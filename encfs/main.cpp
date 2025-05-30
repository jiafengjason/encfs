/*****************************************************************************
 * Author:   Valient Gough <vgough@pobox.com>
 *
 *****************************************************************************
 * Copyright (c) 2003-2004, Valient Gough
 *
 * This library is free software; you can distribute it and/or modify it under
 * the terms of the GNU General Public License (GPL), as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GPL in the file COPYING for more
 * details.
 *
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <sstream>
#include <string>
#ifdef __CYGWIN__
#include <sys/cygwin.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "Context.h"
#include "Error.h"
#include "FileUtils.h"
#include "MemoryPool.h"
#include "autosprintf.h"
#include "config.h"
#include "encfs.h"
#include "fuse.h"
#include "i18n.h"
#include "openssl.h"

/* Arbitrary identifiers for long options that do
 * not have a short version */
#define LONG_OPT_ANNOTATE 513
#define LONG_OPT_NOCACHE 514
#define LONG_OPT_NODATACACHE 515
#define LONG_OPT_NOATTRCACHE 516
#define LONG_OPT_REQUIRE_MAC 517
#define LONG_OPT_INSECURE 518

using namespace std;
using namespace encfs;
using gnu::autosprintf;

namespace encfs {

class DirNode;

// Maximum number of arguments that we're going to pass on to fuse.  Doesn't
// affect how many arguments we can handle, just how many we can pass on..
const int MaxFuseArgs = 32;
/**
 * EncFS_Args stores the parsed command-line arguments
 *
 * See also: struct EncFS_Opts (FileUtils.h), stores internal settings that are
 * derived from the arguments
 */
struct EncFS_Args {
  bool isDaemon;    // true == spawn in background, log to syslog
  bool isThreaded;  // true == threaded
  bool isVerbose;   // false == only enable warning/error messages
  int idleTimeout;  // 0 == idle time in minutes to trigger unmount
  const char *fuseArgv[MaxFuseArgs];
  int fuseArgc;
  std::string syslogTag;  // syslog tag to use when logging using syslog

  std::shared_ptr<EncFS_Opts> opts;

  // for debugging
  // In case someone sends me a log dump, I want to know how what options are
  // in effect.  Not internationalized, since it is something that is mostly
  // useful for me!
  string toString() {
    ostringstream ss;
    ss << (isDaemon ? "(daemon) " : "(fg) ");
    ss << (isThreaded ? "(threaded) " : "(UP) ");
    if (idleTimeout > 0) {
      ss << "(timeout " << idleTimeout << ") ";
    }
    if (opts->checkKey) {
      ss << "(keyCheck) ";
    }
    if (opts->forceDecode) {
      ss << "(forceDecode) ";
    }
    if (opts->ownerCreate) {
      ss << "(ownerCreate) ";
    }
    if (opts->useStdin) {
      ss << "(useStdin) ";
    }
    if (opts->annotate) {
      ss << "(annotate) ";
    }
    if (opts->reverseEncryption) {
      ss << "(reverseEncryption) ";
    }
    if (opts->mountOnDemand) {
      ss << "(mountOnDemand) ";
    }
    if (opts->delayMount) {
      ss << "(delayMount) ";
    }
    for (int i = 0; i < fuseArgc; ++i) {
      ss << fuseArgv[i] << ' ';
    }
    return ss.str();
  }

  EncFS_Args() : opts(new EncFS_Opts()) {}
};

static int oldStderr = STDERR_FILENO;
}  // namespace encfs

static void usage(const char *name) {
  // xgroup(usage)
  cerr << autosprintf(_("Build: encfs version %s"), VERSION) << "\n\n"
       // xgroup(usage)
       << autosprintf(
              _("Usage: %s [options] rootDir mountPoint [-- [FUSE Mount "
                "Options]]"),
              name)
       << "\n\n"
       // xgroup(usage)
       << _("Common Options:\n"
            "  -H\t\t\t"
            "show optional FUSE Mount Options\n"
            "  -s\t\t\t"
            "disable multithreaded operation\n"
            "  -f\t\t\t"
            "run in foreground (don't spawn daemon).\n"
            "\t\t\tError messages will be sent to stderr\n"
            "\t\t\tinstead of syslog.\n")

       // xgroup(usage)
       << _("  -v, --verbose\t\t"
            "verbose: output encfs debug messages\n"
            "  -i, --idle=MINUTES\t"
            "Auto unmount after period of inactivity\n"
            "  --anykey\t\t"
            "Do not verify correct key is being used\n"
            "  --forcedecode\t\t"
            "decode data even if an error is detected\n"
            "\t\t\t(for filesystems using MAC block headers)\n")
       << _("  --public\t\t"
            "act as a typical multi-user filesystem\n"
            "\t\t\t(encfs must be run as root)\n")
       << _("  --reverse\t\t"
            "reverse encryption\n")
       << _("  --reversewrite\t\t"
            "reverse encryption with writes enabled\n")
       << _("  -c, --config=path\t\t"
            "specifies config file (overrides ENV variable)\n")
       << _("  -u, --unmount\t\t"
            "unmounts specified mountPoint\n")

       // xgroup(usage)
       << _("  --extpass=program\tUse external program for password prompt\n"
            "\n"
            "Example, to mount at ~/crypt with raw storage in ~/.crypt :\n"
            "    encfs ~/.crypt ~/crypt\n"
            "\n")
       // xgroup(usage)
       << _("For more information, see the man page encfs(1)") << "\n"
       << endl;
}

static void FuseUsage() {
  // xgroup(usage)
  cerr << _("encfs [options] rootDir mountPoint -- [FUSE Mount Options]\n"
            "valid FUSE Mount Options follow:\n")
       << endl;

  int argc = 2;
  const char *argv[] = {"...", "-h"};
  fuse_main(argc, const_cast<char **>(argv), (fuse_operations *)nullptr,
            nullptr);
}

#define PUSHARG(ARG)                        \
  do {                                      \
    rAssert(out->fuseArgc < MaxFuseArgs);   \
    out->fuseArgv[out->fuseArgc++] = (ARG); \
  } while (false)

static string slashTerminate(const string &src) {
  string result = src;
  if (result[result.length() - 1] != '/') {
    result.append("/");
  }
  return result;
}

static bool processArgs(int argc, char *argv[],
                        const std::shared_ptr<EncFS_Args> &out) {
  // set defaults
  out->isDaemon = true;
  out->isThreaded = true;
  out->isVerbose = false;
  out->idleTimeout = 0;
  out->fuseArgc = 0;
  out->syslogTag = "encfs";
  out->opts->idleTracking = false;
  out->opts->checkKey = true;
  out->opts->forceDecode = false;
  out->opts->ownerCreate = false;
  out->opts->useStdin = false;
  //FUNC-ENV-PASSWORD
  out->opts->useEnv = false;
  out->opts->annotate = false;
  out->opts->reverseEncryption = false;
  out->opts->requireMac = false;
  out->opts->insecure = false;
  out->opts->unmount = false;

  bool useDefaultFlags = true;

  // pass executable name through
  out->fuseArgv[0] = lastPathElement(argv[0]);
  ++out->fuseArgc;

  // leave a space for mount point, as FUSE expects the mount point before
  // any flags
  out->fuseArgv[1] = nullptr;
  ++out->fuseArgc;

  // TODO: can flags be internationalized?
  static struct option long_options[] = {
      {"fuse-debug", 0, nullptr, 'd'},   // Fuse debug mode
      {"forcedecode", 0, nullptr, 'D'},  // force decode
      // {"foreground", 0, 0, 'f'}, // foreground mode (no daemon)
      {"fuse-help", 0, nullptr, 'H'},         // fuse_mount usage
      {"idle", 1, nullptr, 'i'},              // idle timeout
      {"anykey", 0, nullptr, 'k'},            // skip key checks
      {"no-default-flags", 0, nullptr, 'N'},  // don't use default fuse flags
      {"ondemand", 0, nullptr, 'm'},          // mount on-demand
      {"delaymount", 0, nullptr, 'M'},        // delay initial mount until use
      {"public", 0, nullptr, 'P'},            // public mode
      {"extpass", 1, nullptr, 'p'},           // external password program
      // {"single-thread", 0, 0, 's'},  // single-threaded mode
      {"stdinpass", 0, nullptr, 'S'},  // read password from stdin
      {"syslogtag", 1, nullptr, 't'},  // syslog tag
      {"annotate", 0, nullptr,
       LONG_OPT_ANNOTATE},  // Print annotation lines to stderr
      {"nocache", 0, nullptr, LONG_OPT_NOCACHE},         // disable all caching
      {"nodatacache", 0, nullptr, LONG_OPT_NODATACACHE}, // disable data caching
      {"noattrcache", 0, nullptr, LONG_OPT_NOATTRCACHE}, // disable attr caching
      {"verbose", 0, nullptr, 'v'},               // verbose mode
      {"version", 0, nullptr, 'V'},               // version
      {"reverse", 0, nullptr, 'r'},               // reverse encryption
      {"reversewrite", 0, nullptr, 'R'},          // reverse encryption with write enabled
      {"standard", 0, nullptr, '1'},              // standard configuration
      {"paranoia", 0, nullptr, '2'},              // standard configuration
      {"require-macs", 0, nullptr, LONG_OPT_REQUIRE_MAC},  // require MACs
      {"insecure", 0, nullptr, LONG_OPT_INSECURE},// allows to use null data encryption
      {"config", 1, nullptr, 'c'},                // command-line-supplied config location
      {"unmount", 1, nullptr, 'u'},               // unmount
      {nullptr, 0, nullptr, 0}};

  while (true) {
    int option_index = 0;

    // 's' : single-threaded mode
    // 'f' : foreground mode
    // 'v' : verbose mode (same as --verbose)
    // 'd' : fuse debug mode (same as --fusedebug)
    // 'i' : idle-timeout, takes argument
    // 'm' : mount-on-demand
    // 'S' : password from stdin
    // 'E' : password from env
    // 'o' : arguments meant for fuse
    // 't' : syslog tag
    // 'c' : configuration file
    // 'u' : unmount
    //FUNC-ENV-PASSWORD
    int res =
        getopt_long(argc, argv, "HsSfvdmEi:o:t:c:u", long_options, &option_index);

    if (res == -1) {
      break;
    }

    switch (res) {
      case '1':
        out->opts->configMode = Config_Standard;
        break;
      case '2':
        out->opts->configMode = Config_Paranoia;
        break;
      case 's':
        out->isThreaded = false;
        break;
      case 'S':
        out->opts->useStdin = true;
        break;
      //FUNC-ENV-PASSWORD
      case 'E':
        out->opts->useEnv = true;
        break;
      case 't':
        out->syslogTag = optarg;
        break;
      case LONG_OPT_ANNOTATE:
        out->opts->annotate = true;
        break;
      case LONG_OPT_REQUIRE_MAC:
        out->opts->requireMac = true;
        break;
      case LONG_OPT_INSECURE:
        out->opts->insecure = true;
        break;
      case 'c':
        /* Take config file path from command 
         * line instead of ENV variable */
        out->opts->config.assign(optarg);
        break;
      case 'u':
        //we want to log to console, not to syslog, in case of error
        out->isDaemon = false;
        out->opts->unmount = true;
        break;
      case 'f':
        out->isDaemon = false;
        // this option was added in fuse 2.x
        PUSHARG("-f");
        break;
      case 'v':
        out->isVerbose = true;
        break;
      case 'd':
        PUSHARG("-d");
        break;
      case 'i':
        out->idleTimeout = strtol(optarg, (char **)nullptr, 10);
        out->opts->idleTracking = true;
        break;
      case 'k':
        out->opts->checkKey = false;
        break;
      case 'D':
        out->opts->forceDecode = true;
        break;
      case 'r':
        out->opts->reverseEncryption = true;
        /* Reverse encryption does not support writing unless uniqueIV
         * is disabled (expert mode) */
        out->opts->readOnly = true;
        /* By default, the kernel caches file metadata for one second.
         * This is fine for EncFS' normal mode, but for --reverse, this
         * means that the encrypted view will be up to one second out of
         * date.
         * Quoting Goswin von Brederlow:
         * "Caching only works correctly if you implement a disk based
         * filesystem, one where only the fuse process can alter
         * metadata and all access goes only through fuse. Any overlay
         * filesystem where something can change the underlying
         * filesystem without going through fuse can run into
         * inconsistencies."
         * However, disabling the caches causes a factor 3
         * slowdown. If you are concerned about inconsistencies,
         * please use --nocache. */
        break;
      case 'R':
        out->opts->reverseEncryption = true;
         /* At least this is what the user wants, we will see later
            if it is possible */
        out->opts->readOnly = false;
        break;
      case LONG_OPT_NOCACHE:
        /* Disable EncFS block cache
         * Causes reverse grow tests to fail because short reads
         * are returned */
        out->opts->noCache = true;
        /* Disable kernel stat() cache
         * Causes reverse grow tests to fail because stale stat() data
         * is returned */
        PUSHARG("-oattr_timeout=0");
        /* Disable kernel dentry cache
         * Fallout unknown, disabling for safety */
        PUSHARG("-oentry_timeout=0");
#ifdef __CYGWIN__
        // Should be enforced due to attr_timeout=0, but does not seem to work correctly
        // https://github.com/billziss-gh/winfsp/issues/155
        PUSHARG("-oFileInfoTimeout=0");
#endif
        break;
      case LONG_OPT_NODATACACHE:
        out->opts->noCache = true;
        break;
      case LONG_OPT_NOATTRCACHE:
        PUSHARG("-oattr_timeout=0");
        PUSHARG("-oentry_timeout=0");
#ifdef __CYGWIN__
        PUSHARG("-oFileInfoTimeout=0");
#endif
        break;
      case 'm':
        out->opts->mountOnDemand = true;
        break;
      case 'M':
        out->opts->delayMount = true;
        break;
      case 'N':
        useDefaultFlags = false;
        break;
      case 'o':
        PUSHARG("-o");
        PUSHARG(optarg);
        break;
      case 'p':
        out->opts->passwordProgram.assign(optarg);
        break;
      case 'P':
        if (geteuid() != 0) {
          cerr << "option '--public' ignored for non-root user";
        } else {
          out->opts->ownerCreate = true;
          // add 'allow_other' option
          // add 'default_permissions' option (default)
          PUSHARG("-o");
          PUSHARG("allow_other");
        }
        break;
      case 'V':
        // xgroup(usage)
        cerr << autosprintf(_("encfs version %s"), VERSION) << endl;
#if defined(HAVE_XATTR)
        // "--verbose" has to be passed before "--version" for this to work.
        if (out->isVerbose) {
          cerr << "Compiled with : HAVE_XATTR" << endl;
        }
#endif
        exit(EXIT_SUCCESS);
        break;
      case 'H':
        FuseUsage();
        exit(EXIT_SUCCESS);
        break;
      case '?':
        // invalid options..
        break;
      case ':':
        // missing parameter for option..
        break;
      default:
        cerr << "getopt error: " << res;
        break;
    }
  }

  if (!out->isThreaded) {
    PUSHARG("-s");
  }

  // for --unmount, we should have exactly 1 argument - the mount point
  if (out->opts->unmount) {
    if (optind + 1 == argc) {
      // unmountPoint is kept as given by the user : in Cygwin, it is used
      // by pkill to terminate the correct process. We can't then use a
      // Linux-converted Windows-style mountPoint to unmount...
      out->opts->unmountPoint = string(argv[optind++]);
      return true;
    }
    // no mount point specified
    cerr << _("Expecting one argument, aborting.") << endl;
    return false;
  }

  // we should have at least 2 arguments left over - the source directory and
  // the mount point.
  if (optind + 2 <= argc) {
    // both rootDir and mountPoint are assumed to be slash terminated in the
    // rest of the code.
    out->opts->rootDir = slashTerminate(argv[optind++]);
    out->opts->unmountPoint = string(argv[optind++]);
    out->opts->mountPoint = slashTerminate(out->opts->unmountPoint);
  } else {
    // no mount point specified
    cerr << _("Missing one or more arguments, aborting.") << endl;
    return false;
  }

  // If there are still extra unparsed arguments, pass them onto FUSE..
  if (optind < argc) {
    rAssert(out->fuseArgc < MaxFuseArgs);

    while (optind < argc) {
      rAssert(out->fuseArgc < MaxFuseArgs);
      out->fuseArgv[out->fuseArgc++] = argv[optind];
      ++optind;
    }
  }

  // Add default flags unless --no-default-flags was passed
  if (useDefaultFlags) {

    // Expose the underlying stable inode number
    PUSHARG("-o");
    PUSHARG("use_ino");

    // "default_permissions" comes with a performance cost, and only makes
    // sense if "allow_other"" is used.
    // But it works around the issues "open_readonly_workaround" causes,
    // so enable it unconditionally.
    // See https://github.com/vgough/encfs/issues/181 and
    // https://github.com/vgough/encfs/issues/112 for more info.
    PUSHARG("-o");
    PUSHARG("default_permissions");

#if defined(__APPLE__)
    // With OSXFuse, the 'local' flag selects a local filesystem mount icon in
    // Finder.
    PUSHARG("-o");
    PUSHARG("local");
#endif
  }

#ifdef __CYGWIN__
  // Windows users may use Windows paths
  // https://cygwin.com/cygwin-api/cygwin-functions.html
  out->opts->mountPoint = string((char *)cygwin_create_path(CCP_WIN_A_TO_POSIX | CCP_RELATIVE, out->opts->mountPoint.c_str()));
  out->opts->rootDir = string((char *)cygwin_create_path(CCP_WIN_A_TO_POSIX | CCP_RELATIVE, out->opts->rootDir.c_str()));
#endif

  // sanity check
  if (out->isDaemon && (!isAbsolutePath(out->opts->mountPoint.c_str()) ||
                        !isAbsolutePath(out->opts->rootDir.c_str()))) {
    cerr <<
        // xgroup(usage)
        _("When specifying daemon mode, you must use absolute paths "
          "(beginning with '/')")
         << endl;
    return false;
  }

  // the raw directory may not be a subdirectory of the mount point.
  {
    string testMountPoint = out->opts->mountPoint;
    string testRootDir = out->opts->rootDir.substr(0, testMountPoint.length());

    if (testMountPoint == testRootDir) {
      cerr <<
          // xgroup(usage)
          _("The raw directory may not be a subdirectory of the "
            "mount point.")
           << endl;
      return false;
    }
  }

  if (out->opts->delayMount && !out->opts->mountOnDemand) {
    cerr <<
        // xgroup(usage)
        _("You must use mount-on-demand with delay-mount") << endl;
    return false;
  }

  if (out->opts->mountOnDemand && out->opts->passwordProgram.empty()) {
    cerr <<
        // xgroup(usage)
        _("Must set password program when using mount-on-demand") << endl;
    return false;
  }

  // check that the directories exist, or that we can create them..
  if (!isDirectory(out->opts->rootDir.c_str()) &&
      !userAllowMkdir(out->opts->annotate ? 1 : 0, out->opts->rootDir.c_str(),
                      0700)) {
    cerr << _("Unable to locate root directory, aborting.") << endl;
    return false;
  }
#ifdef __CYGWIN__
  if (isDirectory(out->opts->mountPoint.c_str())) {
    cerr << _("Mount point must not exist before mouting, aborting.") << endl;
    return false;
  }
  if ((strncmp(out->opts->mountPoint.c_str(), "/cygdrive/", 10) != 0) ||
      (out->opts->mountPoint.length() != 12)) {
    cerr << _("A drive is prefered for mouting, ")
         << _("so a path like X: (or /cygdrive/x) should rather be used. ")
         << _("Mounting anyway.")  << endl;
  }
#else
  if (!isDirectory(out->opts->mountPoint.c_str()) &&
      !userAllowMkdir(out->opts->annotate ? 2 : 0,
                      out->opts->mountPoint.c_str(), 0700)) {
    cerr << _("Unable to locate mount point, aborting.") << endl;
    return false;
  }
#endif

  // fill in mount path for fuse
  out->fuseArgv[1] = out->opts->mountPoint.c_str();
#ifdef __CYGWIN__
  if ((strncmp(out->opts->mountPoint.c_str(), "/cygdrive/", 10) == 0) &&
      (out->opts->mountPoint.length() == 12)) {
    out->opts->cygDrive = out->opts->mountPoint.substr(10,1).append(":");
    out->fuseArgv[1] = out->opts->cygDrive.c_str();
  }
#endif

  return true;
}

static void *idleMonitor(void *);

void *encfs_init(fuse_conn_info *conn) {
  auto *ctx = (EncFS_Context *)fuse_get_context()->private_data;

  // set fuse connection options
  conn->async_read = 1u;

#ifdef __CYGWIN__
  // WinFsp needs this to partially handle read-only FS
  // See https://github.com/billziss-gh/winfsp/issues/157 for details
  if (ctx->opts->readOnly) {
    conn->want |= (conn->capable & FSP_FUSE_CAP_READ_ONLY);
  }
#endif

  // if an idle timeout is specified, then setup a thread to monitor the
  // filesystem.
  if (ctx->args->idleTimeout > 0) {
    VLOG(1) << "starting idle monitoring thread";
    ctx->running = true;

    int res =
        pthread_create(&ctx->monitorThread, nullptr, idleMonitor, (void *)ctx);
    if (res != 0) {
      RLOG(ERROR) << "error starting idle monitor thread, "
                     "res = "
                  << res << ", " << strerror(res);
    }
  }

  if (ctx->args->isDaemon && oldStderr >= 0) {
    VLOG(1) << "Closing stderr";
    close(oldStderr);
    oldStderr = -1;
  }

  return (void *)ctx;
}

int main(int argc, char *argv[]) {
#if defined(ENABLE_NLS) && defined(LOCALEDIR)
  setlocale(LC_ALL, "");
  bindtextdomain(PACKAGE, LOCALEDIR);
  textdomain(PACKAGE);
#endif

  if(!encfs::init_encfs_pidinfo())
  {
    cerr << "Init encfs pid info failed" << endl;
    return EXIT_FAILURE;
  }

  // anything that comes from the user should be considered tainted until
  // we've processed it and only allowed through what we support.
  std::shared_ptr<EncFS_Args> encfsArgs(new EncFS_Args);
  for (int i = 0; i < MaxFuseArgs; ++i) {
    encfsArgs->fuseArgv[i] = nullptr;  // libfuse expects null args..
  }

  if (argc == 1 || !processArgs(argc, argv, encfsArgs)) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  encfs::initLogging(encfsArgs->isVerbose, encfsArgs->isDaemon);
  ELPP_INITIALIZE_SYSLOG(encfsArgs->syslogTag.c_str(), LOG_PID, LOG_USER);

  // Let's unmount if requested
  if (encfsArgs->opts->unmount) {
    // We use cout here to avoid logging to stderr (and to mess-up tests output)
    cout << "Filesystem unmounting: " << encfsArgs->opts->unmountPoint << endl;
    unmountFS(encfsArgs->opts->unmountPoint.c_str());
    return 0;
  }

  VLOG(1) << "Root directory: " << encfsArgs->opts->rootDir;
  VLOG(1) << "Fuse arguments: " << encfsArgs->toString();

  fuse_operations encfs_oper;
  // in case this code is compiled against a newer FUSE library and new
  // members have been added to fuse_operations, make sure they get set to
  // 0..
  memset(&encfs_oper, 0, sizeof(fuse_operations));

  encfs_oper.getattr = encfs_getattr;
  encfs_oper.readlink = encfs_readlink;
  encfs_oper.readdir = encfs_readdir;
  encfs_oper.mknod = encfs_mknod;
  encfs_oper.mkdir = encfs_mkdir;
  encfs_oper.unlink = encfs_unlink;
  encfs_oper.rmdir = encfs_rmdir;
  encfs_oper.symlink = encfs_symlink;
  encfs_oper.rename = encfs_rename;
  encfs_oper.link = encfs_link;
  encfs_oper.chmod = encfs_chmod;
  encfs_oper.chown = encfs_chown;
  encfs_oper.truncate = encfs_truncate;
  encfs_oper.utime = encfs_utime;  // deprecated for utimens
  encfs_oper.open = encfs_open;
  encfs_oper.read = encfs_read;
  encfs_oper.write = encfs_write;
  encfs_oper.statfs = encfs_statfs;
  encfs_oper.flush = encfs_flush;
  encfs_oper.release = encfs_release;
  encfs_oper.fsync = encfs_fsync;
#ifdef HAVE_XATTR
  encfs_oper.setxattr = encfs_setxattr;
  encfs_oper.getxattr = encfs_getxattr;
  encfs_oper.listxattr = encfs_listxattr;
  encfs_oper.removexattr = encfs_removexattr;
#endif  // HAVE_XATTR
  // encfs_oper.opendir = encfs_opendir;
  // encfs_oper.readdir = encfs_readdir;
  // encfs_oper.releasedir = encfs_releasedir;
  // encfs_oper.fsyncdir = encfs_fsyncdir;
  encfs_oper.init = encfs_init;
  // encfs_oper.access = encfs_access;
  encfs_oper.create = encfs_create;
  encfs_oper.ftruncate = encfs_ftruncate;
  encfs_oper.fgetattr = encfs_fgetattr;
  // encfs_oper.lock = encfs_lock;
  encfs_oper.utimens = encfs_utimens;
  // encfs_oper.bmap = encfs_bmap;

  openssl_init(encfsArgs->isThreaded);

  // context is not a smart pointer because it will live for the life of
  // the filesystem.
  auto ctx = std::make_shared<EncFS_Context>();
  ctx->publicFilesystem = encfsArgs->opts->ownerCreate;
  RootPtr rootInfo = initFS(ctx.get(), encfsArgs->opts);

  int returnCode = EXIT_FAILURE;

  if (rootInfo) {
    // turn off delayMount, as our prior call to initFS has already
    // respected any delay, and we want future calls to actually
    // mount.
    encfsArgs->opts->delayMount = false;

    // set the globally visible root directory node
    ctx->setRoot(rootInfo->root);
    ctx->args = encfsArgs;
    ctx->opts = encfsArgs->opts;

    if (!encfsArgs->isThreaded && encfsArgs->idleTimeout > 0) {
      // xgroup(usage)
      cerr << _("Note: requested single-threaded mode, but an idle\n"
                "timeout was specified.  The filesystem will operate\n"
                "single-threaded, but threads will still be used to\n"
                "implement idle checking.")
           << endl;
    }

    // reset umask now, since we don't want it to interfere with the
    // pass-thru calls..
    umask(0);

    if (encfsArgs->isDaemon) {
      // keep around a pointer just in case we end up needing it to
      // report a fatal condition later (fuse_main exits unexpectedly)...
      oldStderr = dup(STDERR_FILENO);
    }

    try {
      time_t startTime, endTime;

      if (encfsArgs->opts->annotate) {
        cerr << "$STATUS$ fuse_main_start" << endl;
      }

      // FIXME: workaround for fuse_main returning an error on normal
      // exit.  Only print information if fuse_main returned
      // immediately..
      time(&startTime);

      // fuse_main returns an error code in newer versions of fuse..
      int res = fuse_main(encfsArgs->fuseArgc,
                          const_cast<char **>(encfsArgs->fuseArgv), &encfs_oper,
                          (void *)ctx.get());

      time(&endTime);

      if (encfsArgs->opts->annotate) {
        cerr << "$STATUS$ fuse_main_end" << endl;
      }

      if (res == 0) {
        returnCode = EXIT_SUCCESS;
      }

      if (res != 0 && encfsArgs->isDaemon && (oldStderr >= 0) &&
          (endTime - startTime <= 1)) {
        // the users will not have seen any message from fuse, so say a
        // few words in libfuse's memory..
        FILE *out = fdopen(oldStderr, "a");
        // xgroup(usage)
        fputs(_("fuse failed.  Common problems:\n"
                " - fuse kernel module not installed (modprobe fuse)\n"
                " - invalid options -- see usage message\n"),
              out);
        fclose(out);
      }
    } catch (std::exception &ex) {
      RLOG(ERROR) << "Internal error: Caught exception from main loop: "
                  << ex.what();
    } catch (...) {
      RLOG(ERROR) << "Internal error: Caught unexpected exception";
    }

    if (ctx->args->idleTimeout > 0) {
      ctx->running = false;
      // wake up the thread if it is waiting..
      VLOG(1) << "waking up monitoring thread";
      pthread_mutex_lock(&ctx->wakeupMutex);
      pthread_cond_signal(&ctx->wakeupCond);
      pthread_mutex_unlock(&ctx->wakeupMutex);
      VLOG(1) << "joining with idle monitoring thread";
      pthread_join(ctx->monitorThread, nullptr);
      VLOG(1) << "join done";
    }
  }

  // cleanup so that we can check for leaked resources..
  rootInfo.reset();
  ctx->setRoot(std::shared_ptr<DirNode>());

  MemoryPool::destroyAll();
  openssl_shutdown(encfsArgs->isThreaded);

  return returnCode;
}

/*
    Idle monitoring thread.  This is only used when idle monitoring is enabled.
    It will cause the filesystem to be automatically unmounted (causing us to
    commit suicide) if the filesystem stays idle too long.  Idle time is only
    checked if there are no open files, as I don't want to risk problems by
    having the filesystem unmounted from underneath open files!
*/
const int ActivityCheckInterval = 10;

static void *idleMonitor(void *_arg) {
  auto *ctx = (EncFS_Context *)_arg;
  std::shared_ptr<EncFS_Args> arg = ctx->args;

  const int timeoutCycles = 60 * arg->idleTimeout / ActivityCheckInterval;

  bool unmountres = false;

  // We will notify when FS will be unmounted, so notify that it has just been
  // mounted
  RLOG(INFO) << "Filesystem mounted: " << arg->opts->unmountPoint;

  pthread_mutex_lock(&ctx->wakeupMutex);

  while (ctx->running) {
    unmountres = ctx->usageAndUnmount(timeoutCycles);
    if (unmountres) {
      break;
    }

    struct timeval currentTime;
    gettimeofday(&currentTime, nullptr);
    struct timespec wakeupTime;
    wakeupTime.tv_sec = currentTime.tv_sec + ActivityCheckInterval;
    wakeupTime.tv_nsec = currentTime.tv_usec * 1000;
    pthread_cond_timedwait(&ctx->wakeupCond, &ctx->wakeupMutex, &wakeupTime);
  }

  pthread_mutex_unlock(&ctx->wakeupMutex);

  // If we are here FS has been unmounted, so if the idleMonitor did not unmount itself,
  // let's notify (certainly due to a kill signal, a manual unmount...)
  if (!unmountres) {
    RLOG(INFO) << "Filesystem unmounted: " << arg->opts->unmountPoint;
  }

  VLOG(1) << "Idle monitoring thread exiting";

  return nullptr;
}
