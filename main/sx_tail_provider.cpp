
#include <stdio.h>
#include <assert.h>
#include <getopt.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdint>
#include <cstdlib>
#include <cerrno>


struct File_spec
{
  /* The actual file name, or "-" for stdin.  */
  char *name;

  /* Attributes of the file the last time we checked.  */
  off_t size;
  struct timespec mtime;
  dev_t dev;
  ino_t ino;
  mode_t mode;

  /* The specified name initially referred to a directory or some other
     type for which tail isn't meaningful.  Unlike for a permission problem
     (tailable, below) once this is set, the name is not checked ever again.  */
  bool ignore;

  /* See the description of fremote.  */
  bool remote;

  /* A file is tailable if it exists, is readable, and is of type
     IS_TAILABLE_FILE_TYPE.  */
  bool tailable;

  /* File descriptor on which the file is open; -1 if it's not open.  */
  int fd;

  /* The value of errno seen last time we checked this file.  */
  int errnum;

  /* 1 if O_NONBLOCK is clear, 0 if set, -1 if not known.  */
  int blocking;

#if HAVE_INOTIFY
  /* The watch descriptor used by inotify.  */
  int wd;

  /* The parent directory watch descriptor.  It is used only
   * when Follow_name is used.  */
  int parent_wd;

  /* Offset in NAME of the basename part.  */
  size_t basename_start;
#endif

  /* See description of DEFAULT_MAX_N_... below.  */
  uintmax_t n_unchanged_stats;
};

static char const *
pretty_name (struct File_spec const *f)
{
  return (STREQ (f->name, "-") ? _("standard input") : f->name);
}

static void
tail_forever (struct File_spec *f, size_t n_files, double sleep_interval)
{
  size_t last;
  bool writer_is_dead = false;

  last = n_files - 1;

  while (true)
    {
      size_t i;
      bool any_input = false;

      for (i = 0; i < n_files; i++)
        {
          int fd;
          char const *name;
          mode_t mode;
          struct stat stats;
          uintmax_t bytes_read;

          if (f[i].ignore)
            continue;

          if (f[i].fd < 0)
            {
              recheck (&f[i], blocking);
              continue;
            }

          fd = f[i].fd;
          name = pretty_name (&f[i]);
          mode = f[i].mode;


          if (!f[i].blocking)
            {
              if (fstat (fd, &stats) != 0)
                {
                  f[i].fd = -1;
                  f[i].errnum = errno;
                  error (0, errno, "%s", quotef (name));
                  close (fd); /* ignore failure */
                  continue;
                }

              if (f[i].mode == stats.st_mode
                  && (! S_ISREG (stats.st_mode) || f[i].size == stats.st_size)
                  && timespec_cmp (f[i].mtime, get_stat_mtime (&stats)) == 0)
                {
                  if ((max_n_unchanged_stats_between_opens
                       <= f[i].n_unchanged_stats++)
                      && follow_mode == Follow_name)
                    {
                      recheck (&f[i], f[i].blocking);
                      f[i].n_unchanged_stats = 0;
                    }
                  continue;
                }

              /* This file has changed.  Print out what we can, and
                 then keep looping.  */

              f[i].mtime = get_stat_mtime (&stats);
              f[i].mode = stats.st_mode;

              /* reset counter */
              f[i].n_unchanged_stats = 0;

              /* XXX: This is only a heuristic, as the file may have also
                 been truncated and written to if st_size >= size
                 (in which case we ignore new data <= size).  */
              if (S_ISREG (mode) && stats.st_size < f[i].size)
                {
                  error (0, 0, _("%s: file truncated"), quotef (name));
                  /* Assume the file was truncated to 0,
                     and therefore output all "new" data.  */
                  xlseek (fd, 0, SEEK_SET, name);
                  f[i].size = 0;
                }

              if (i != last)
                {
                  if (print_headers)
                    write_header (name);
                  last = i;
                }
            }

          /* Don't read more than st_size on networked file systems
             because it was seen on glusterfs at least, that st_size
             may be smaller than the data read on a _subsequent_ stat call.  */
          uintmax_t bytes_to_read;
          if (f[i].blocking)
            bytes_to_read = COPY_A_BUFFER;
          else if (S_ISREG (mode) && f[i].remote)
            bytes_to_read = stats.st_size - f[i].size;
          else
            bytes_to_read = COPY_TO_EOF;

          bytes_read = dump_remainder (false, name, fd, bytes_to_read);

          any_input |= (bytes_read != 0);
          f[i].size += bytes_read;
        }

      if (! any_live_files (f, n_files))
        {
          error (0, 0, _("no files remaining"));
          break;
        }

      if ((!any_input || blocking) && fflush (stdout) != 0)
        die (EXIT_FAILURE, errno, _("write error"));

      check_output_alive ();

      /* If nothing was read, sleep and/or check for dead writers.  */
      if (!any_input)
        {
          if (writer_is_dead)
            break;

          /* Once the writer is dead, read the files once more to
             avoid a race condition.  */
          writer_is_dead = (pid != 0
                            && kill (pid, 0) != 0
                            /* Handle the case in which you cannot send a
                               signal to the writer, so kill fails and sets
                               errno to EPERM.  */
                            && errno != EPERM);

          if (!writer_is_dead && xnanosleep (sleep_interval))
            die (EXIT_FAILURE, errno, _("cannot read realtime clock"));

        }
    }
}


int
main (int argc, char **argv)
{
  enum header_mode header_mode = multiple_files;
  bool ok = true;
  /* If from_start, the number of items to skip before printing; otherwise,
     the number of items at the end of the file to print.  Although the type
     is signed, the value is never negative.  */
  uintmax_t n_units = DEFAULT_N_LINES;
  size_t n_files;
  char **file;
  struct File_spec *F;
  size_t i;
  bool obsolete_option;

  /* The number of seconds to sleep between iterations.
     During one iteration, every file name or descriptor is checked to
     see if it has changed.  */
  double sleep_interval = 1.0;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  have_read_stdin = false;

  count_lines = true;
  forever = from_start = print_headers = false;
  line_end = '\n';
  obsolete_option = parse_obsolete_option (argc, argv, &n_units);
  argc -= obsolete_option;
  argv += obsolete_option;
  parse_options (argc, argv, &n_units, &header_mode, &sleep_interval);

  /* To start printing with item N_UNITS from the start of the file, skip
     N_UNITS - 1 items.  'tail -n +0' is actually meaningless, but for Unix
     compatibility it's treated the same as 'tail -n +1'.  */
  if (from_start)
    {
      if (n_units)
        --n_units;
    }

  IF_LINT (assert (0 <= argc));

  if (optind < argc)
    {
      n_files = argc - optind;
      file = argv + optind;
    }
  else
    {
      static char *dummy_stdin = (char *) "-";
      n_files = 1;
      file = &dummy_stdin;
    }

  {
    bool found_hyphen = false;

    for (i = 0; i < n_files; i++)
      if (STREQ (file[i], "-"))
        found_hyphen = true;

    /* When following by name, there must be a name.  */
    if (found_hyphen && follow_mode == Follow_name)
      die (EXIT_FAILURE, 0, _("cannot follow %s by name"), quoteaf ("-"));

    /* When following forever, and not using simple blocking, warn if
       any file is '-' as the stats() used to check for input are ineffective.
       This is only a warning, since tail's output (before a failing seek,
       and that from any non-stdin files) might still be useful.  */
    if (forever && found_hyphen)
      {
        struct stat in_stat;
        bool blocking_stdin;
        blocking_stdin = (pid == 0 && follow_mode == Follow_descriptor
                          && n_files == 1 && ! fstat (STDIN_FILENO, &in_stat)
                          && ! S_ISREG (in_stat.st_mode));

        if (! blocking_stdin && isatty (STDIN_FILENO))
          error (0, 0, _("warning: following standard input"
                         " indefinitely is ineffective"));
      }
  }

  /* Don't read anything if we'll never output anything.  */
  if (! n_units && ! forever && ! from_start)
    return EXIT_SUCCESS;

  F = xnmalloc (n_files, sizeof *F);
  for (i = 0; i < n_files; i++)
    F[i].name = file[i];

  if (header_mode == always
      || (header_mode == multiple_files && n_files > 1))
    print_headers = true;

  xset_binary_mode (STDOUT_FILENO, O_BINARY);

  for (i = 0; i < n_files; i++)
    ok &= tail_file (&F[i], n_units);

  if (forever && ignore_fifo_and_pipe (F, n_files))
    {
      /* If stdout is a fifo or pipe, then monitor it
         so that we exit if the reader goes away.  */
      struct stat out_stat;
      if (fstat (STDOUT_FILENO, &out_stat) < 0)
        die (EXIT_FAILURE, errno, _("standard output"));
      monitor_output = (S_ISFIFO (out_stat.st_mode)
                        || (HAVE_FIFO_PIPES != 1 && isapipe (STDOUT_FILENO)));

#if HAVE_INOTIFY
      /* tailable_stdin() checks if the user specifies stdin via  "-",
         or implicitly by providing no arguments. If so, we won't use inotify.
         Technically, on systems with a working /dev/stdin, we *could*,
         but would it be worth it?  Verifying that it's a real device
         and hooked up to stdin is not trivial, while reverting to
         non-inotify-based tail_forever is easy and portable.
         any_remote_file() checks if the user has specified any
         files that reside on remote file systems.  inotify is not used
         in this case because it would miss any updates to the file
         that were not initiated from the local system.
         any_non_remote_file() checks if the user has specified any
         files that don't reside on remote file systems.  inotify is not used
         if there are no open files, as we can't determine if those file
         will be on a remote file system.
         any_symlinks() checks if the user has specified any symbolic links.
         inotify is not used in this case because it returns updated _targets_
         which would not match the specified names.  If we tried to always
         use the target names, then we would miss changes to the symlink itself.
         ok is false when one of the files specified could not be opened for
         reading.  In this case and when following by descriptor,
         tail_forever_inotify() cannot be used (in its current implementation).
         FIXME: inotify doesn't give any notification when a new
         (remote) file or directory is mounted on top a watched file.
         When follow_mode == Follow_name we would ideally like to detect that.
         Note if there is a change to the original file then we'll
         recheck it and follow the new file, or ignore it if the
         file has changed to being remote.
         FIXME-maybe: inotify has a watch descriptor per inode, and hence with
         our current hash implementation will only --follow data for one
         of the names when multiple hardlinked files are specified, or
         for one name when a name is specified multiple times.  */
      if (!disable_inotify && (tailable_stdin (F, n_files)
                               || any_remote_file (F, n_files)
                               || ! any_non_remote_file (F, n_files)
                               || any_symlinks (F, n_files)
                               || any_non_regular_fifo (F, n_files)
                               || (!ok && follow_mode == Follow_descriptor)))
        disable_inotify = true;

      if (!disable_inotify)
        {
          int wd = inotify_init ();
          if (0 <= wd)
            {
              /* Flush any output from tail_file, now, since
                 tail_forever_inotify flushes only after writing,
                 not before reading.  */
              if (fflush (stdout) != 0)
                die (EXIT_FAILURE, errno, _("write error"));

              if (! tail_forever_inotify (wd, F, n_files, sleep_interval))
                return EXIT_FAILURE;
            }
          error (0, errno, _("inotify cannot be used, reverting to polling"));

          /* Free resources as this process can be long lived,
            and we may have exhausted system resources above.  */

          for (i = 0; i < n_files; i++)
            {
              /* It's OK to remove the same watch multiple times,
                ignoring the EINVAL from redundant calls.  */
              if (F[i].wd != -1)
                inotify_rm_watch (wd, F[i].wd);
              if (F[i].parent_wd != -1)
                inotify_rm_watch (wd, F[i].parent_wd);
            }
        }
#endif
      disable_inotify = true;
      tail_forever (F, n_files, sleep_interval);
    }

  IF_LINT (free (F));

  if (have_read_stdin && close (STDIN_FILENO) < 0)
    die (EXIT_FAILURE, errno, "-");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
