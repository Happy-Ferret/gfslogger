/*
###############################################################################
#                                                                             #
#   gfslogger v1.0                                                            #
#   Subscribes to Mac OS X 10.4 fsevents and displays all filesystem changes. #
#                                                                             #
#   Copyright (C) 2016, 2005 Rian Hunter, rian@alum.mit.edu                   #
#                                                                             #
#   This program is free software; you can redistribute it and/or modify      #
#   it under the terms of the GNU General Public License as published by      #
#   the Free Software Foundation; either version 2 of the License, or         #
#   (at your option) any later version.                                       #
#                                                                             #
#   This program is distributed in the hope that it will be useful,           #
#   but WITHOUT ANY WARRANTY; without even the implied warranty of            #
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             #
#   GNU General Public License for more details.                              #
#                                                                             #
#   You should have received a copy of the GNU General Public License         #
#   along with this program; if not, write to the Free Software	              #
#   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA #
#                                                                             #
###############################################################################
*/
// for open(2)
#include <fcntl.h>

// for ioctl(2)
#include <sys/ioctl.h>
#include <sys/sysctl.h>

// for read(2)
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

// for printf(3)
#include <stdio.h>

// for exit(3)
#include <stdlib.h>

// for strncpy(3)
#include <string.h>

// for getpwuid(3)
#include <pwd.h>

// for getgrgid(3)
#include <grp.h>

// for S_IS*(3)
#include <sys/stat.h>

// duh.
#include "fsevents.h"

static void die(int);
static void process_event_data(void *, int);
static void get_process_name(pid_t, char *, int);
static void get_mode_string(int32_t, char *);
static char *get_vnode_type(int32_t);

char large_buf[0x2000];

// activates self as fsevent listener and displays fsevents
// must be run as root!! (at least on Mac OS X 10.4)
int main() {
  int newfd, fd, n;
  signed char event_list[FSE_MAX_EVENTS];
  fsevent_clone_args retrieve_ioctl;

  event_list[FSE_CREATE_FILE]         = FSE_REPORT;
  event_list[FSE_DELETE]              = FSE_REPORT;
  event_list[FSE_STAT_CHANGED]        = FSE_REPORT;
  event_list[FSE_RENAME]              = FSE_REPORT;
  event_list[FSE_CONTENT_MODIFIED]    = FSE_REPORT;
  event_list[FSE_EXCHANGE]            = FSE_REPORT;
  event_list[FSE_FINDER_INFO_CHANGED] = FSE_REPORT;
  event_list[FSE_CREATE_DIR]          = FSE_REPORT;
  event_list[FSE_CHOWN]               = FSE_REPORT;
  event_list[FSE_XATTR_MODIFIED]      = FSE_REPORT;
  event_list[FSE_XATTR_REMOVED]       = FSE_REPORT;
  event_list[FSE_DOCID_CREATED]       = FSE_REPORT;
  event_list[FSE_DOCID_CHANGED]       = FSE_REPORT;

  fd = open("/dev/fsevents", 0, 2);
  if (fd < 0)
    die(1);

  retrieve_ioctl.event_list = event_list;
  retrieve_ioctl.num_events = sizeof(event_list);
  retrieve_ioctl.event_queue_depth = 0x400;
  retrieve_ioctl.fd = &newfd;

  if (ioctl(fd, FSEVENTS_CLONE, &retrieve_ioctl) < 0) {
    die(1);
  }

  close(fd);

  printf("gfslogger ready\n");

  // note: you must read at least 2048 bytes at a time on this fd, to get data.
  // also you read quick! newer events can be lost in the internal kernel event
  // buffer if you take too long to get events. thats why buffer is so large:
  // less read calls.
  while ((n = read(newfd, large_buf, sizeof(large_buf))) > 0) {
    process_event_data(large_buf, n);
  }

  return 0;
}

/* event structure in mem:

event type: 4 bytes
event pid: sizeof(pid_t) (4 on darwin) bytes
args:
  argtype: 2 bytes
  arglen: 2 bytes
  argdata: arglen bytes
lastarg:
  argtype: 2 bytes = 0xb33f

*/

// parses the incoming event data and displays it in a friendly way
static void process_event_data(void *in_buf, int size) {
  int pos = 0;
  pid_t pid;
  uid_t uid;
  dev_t device;
  gid_t gid;
  int32_t mode;
  char buffer[0x100];
  u_int16_t argtype;
  u_int16_t arglen;

  printf("=> received %d bytes\n", size);

  do {
    int32_t type;
    printf("# Event\n");

    memcpy(&type, in_buf + pos, sizeof(type));

    printf("  type           = ");
    switch (type) {
    case FSE_CREATE_FILE:
      printf("CREATE FILE");
      break;
    case FSE_DELETE:
      printf("DELETE");
      break;
    case FSE_STAT_CHANGED:
      printf("STAT CHANGED");
      break;
    case FSE_RENAME:
      printf("RENAME");
      break;
    case FSE_CONTENT_MODIFIED:
      printf("CONTENT MODIFIED");
      break;
    case FSE_EXCHANGE:
      printf("EXCHANGE");
      break;
    case FSE_FINDER_INFO_CHANGED:
      printf("FINDER INFO CHANGED");
      break;
    case FSE_CREATE_DIR:
      printf("CREATE DIR");
      break;
    case FSE_CHOWN:
      printf("CHOWN");
      break;
    case FSE_XATTR_MODIFIED:
      printf("XATTR MODIFIED");
      break;
    case FSE_XATTR_REMOVED:
      printf("XATTR REMOVED");
      break;
    case FSE_DOCID_CREATED:
      printf("DOCID CREATED");
      break;
    case FSE_DOCID_CHANGED:
      printf("DOCID CHANGED");
      break;
    case FSE_INVALID: default:
      printf("INVALID");
      return; // <----------we return if invalid type (give up on this data)
      break;
    }
    printf("\n");
    pos += 4;

    memcpy(&pid, in_buf + pos, sizeof(pid));

    get_process_name(pid, buffer, sizeof(buffer));

    printf("  pid            = %d (%s)\n", pid, buffer);
    pos += sizeof(pid_t);

    printf("  # Details\n"
           "    # type       len  data\n");

    while(1) {
      memcpy(&argtype, in_buf + pos, sizeof(argtype));
      pos += 2;

      if (FSE_ARG_DONE == argtype) {
        printf("    DONE (0x%x)\n", argtype);
        break;
      }

      memcpy(&arglen, in_buf + pos, sizeof(arglen));
      pos += 2;

      switch(argtype) {
      case FSE_ARG_VNODE:
        printf("    VNODE%11d  path   = %s\n", arglen, (in_buf + pos));
        break;
      case FSE_ARG_STRING:
        printf("    STRING%10d  string = %s\n", arglen, (in_buf + pos));
        break;
      case FSE_ARG_PATH: // not in kernel
        printf("    PATH%12d  path   = %s\n", arglen, (in_buf + pos));
        break;
      case FSE_ARG_INT32:
        if (arglen == sizeof(int32_t)) {
          int32_t val;
          memcpy(&val, in_buf + pos, sizeof(val));
          printf("    INT32%11d  int32  = %d\n",
                 arglen, val);
        }
        else {
          printf("    INT32%11d  int32\n", arglen);
        }
        break;
      case FSE_ARG_INT64: // not supported in kernel yet
        if (arglen == sizeof(int64_t)) {
          int64_t val;
          memcpy(&val, in_buf + pos, sizeof(val));
          printf("    INT64%11d  int64  = %lld\n",
                 arglen, val);
        }
        else {
          printf("    INT64%11d  int64\n", arglen);
        }
        break;
      case FSE_ARG_RAW: // just raw bytes, can't display
        printf("    RAW%13d  raw\n",
               arglen);
        break;
      case FSE_ARG_INO:
        {
          if (arglen == sizeof(uint32_t)) {
            uint32_t val;
            memcpy(&val, in_buf + pos, sizeof(val));
            printf("    INODE%11d  ino    = %u\n",
                   arglen, val);
          }
          else if (arglen == sizeof(uint64_t)) {
            uint64_t val;
            memcpy(&val, in_buf + pos, sizeof(val));
            printf("    INODE%11d  ino    = %llu\n",
                   arglen, val);
          }
          else {
            printf("    INODE%11d  ino\n",
                   arglen);
          }
        }
        break;
      case FSE_ARG_UID:
        if (arglen == sizeof(uid)) {
          memcpy(&uid, in_buf + pos, sizeof(uid));
          printf("    UID%13d  uid    = %d (%s)\n",
                 arglen, uid, (getpwuid(uid))->pw_name);
        }
        else {
          printf("    UID%13d  uid\n", arglen);
        }
        break;
      case FSE_ARG_DEV:
        if (arglen == sizeof(device)) {
          memcpy(&device, in_buf + pos, sizeof(device));
          printf("    DEV%13d  dev    = 0x%x (major %d, minor %d)\n",
                 arglen, device,
                 (device >> 24) & 0x0FFFFFF, device & 0x0FFFFFF);
        }
        else {
          printf("    DEV%13d  dev\n", arglen);
        }
        break;
      case FSE_ARG_MODE:
        if (arglen == sizeof(mode)) {
          memcpy(&mode, in_buf + pos, sizeof(mode));
          get_mode_string(mode, buffer);
          printf("    MODE%12d  mode   = %s (0x%06x, vnode type %s)\n",
                 arglen, buffer, mode, get_vnode_type(mode));
        }
        else {
          printf("    MODE%12d  mode\n", arglen);
        }
        break;
      case FSE_ARG_GID:
        if (arglen == sizeof(gid)) {
          memcpy(&gid, in_buf + pos, sizeof(gid));
          printf("    GID%13d  gid    = %d (%s)\n",
                 arglen, gid, (getgrgid(gid))->gr_name);
        }
        else {
          printf("    GID%13d  gid\n", arglen);
        }
        break;
      default:
        return; // <----------we return if invalid type (give up on this data)
        break;
      }
      pos += arglen;
    }

  } while (pos < size);

  return;
}

// dies with optional error message
static void die(int p) {
  if (p)
    perror(NULL);
  exit(1);
}

// returns a process name in out_buf
// mac os x specific
static void get_process_name(pid_t process_id, char *out_buf, int outsize) {
  int mib[4];
  size_t len;
  struct kinfo_proc kp;

  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC;
  mib[2] = KERN_PROC_PID;
  mib[3] = process_id;

  len = sizeof(kp);
  if (sysctl(mib, 4, &kp, &len, NULL, 0) == -1) {
    strncpy(out_buf, "exited?", outsize);
  } else {
    strncpy(out_buf, kp.kp_proc.p_comm, outsize);
  }
}

// converts mode number to ls-style mode string
static void get_mode_string(int32_t mode, char *buf) {
  buf[10] = '\0';
  buf[9] = mode & 0x01 ? 'x' : '-';
  buf[8] = mode & 0x02 ? 'w' : '-';
  buf[7] = mode & 0x04 ? 'r' : '-';
  buf[6] = mode & 0x08 ? 'x' : '-';
  buf[5] = mode & 0x10 ? 'w' : '-';
  buf[4] = mode & 0x20 ? 'r' : '-';
  buf[3] = mode & 0x40 ? 'x' : '-';
  buf[2] = mode & 0x80 ? 'w' : '-';
  buf[1] = mode & 0x100 ? 'r' : '-';

  // ls style mode string
  if (S_ISFIFO(mode)) {
    buf[0] = 'p';
  } else if (S_ISCHR(mode)) {
    buf[0] = 'c';
  } else if (S_ISDIR(mode)) {
    buf[0] = 'd';
  } else if (S_ISBLK(mode)) {
    buf[0] = 'b';
  } else if (S_ISLNK(mode)) {
    buf[0] = 'l';
  } else if (S_ISSOCK(mode)) {
    buf[0] = 's';
  } else {
    buf[0] = '-';
  }
}

// just returns a string representation of a node type
static char *get_vnode_type(int32_t mode) {
  char *str_to_ret;

  if (S_ISFIFO(mode)) {
    str_to_ret = "VFIFO";
  } else if (S_ISCHR(mode)) {
    str_to_ret = "VCHR";
  } else if (S_ISDIR(mode)) {
    str_to_ret = "VDIR";
  } else if (S_ISBLK(mode)) {
    str_to_ret = "VBLK";
  } else if (S_ISLNK(mode)) {
    str_to_ret = "VLNK";
  } else if (S_ISSOCK(mode)) {
    str_to_ret = "VSOCK";
  } else {
    str_to_ret = "VREG";
  }

  return str_to_ret;
}
