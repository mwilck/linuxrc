/*
 *
 * install.c           Handling of installation
 *
 * Copyright (c) 1996-2002  Hubert Mantel, SuSE Linux AG  (mantel@suse.de)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <netdb.h>
#include <errno.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/swap.h>
#include <sys/socket.h>
#include <sys/reboot.h>
#include <sys/vfs.h>
#include <arpa/inet.h>

#include <hd.h>

#include "global.h"
#include "linuxrc.h"
#include "text.h"
#include "util.h"
#include "dialog.h"
#include "window.h"
#include "net.h"
#include "display.h"
#include "rootimage.h"
#include "module.h"
#include "keyboard.h"
#include "file.h"
#include "info.h"
#include "ftp.h"
#include "install.h"
#include "settings.h"
#include "auto2.h"
#include "fstype.h"
#include "url.h"

#ifndef MNT_DETACH
#define MNT_DETACH	(1 << 1)
#endif

static char  inst_rootimage_tm [MAX_FILENAME];

static int inst_mount_harddisk(void);
static int   inst_try_cdrom           (char *device_tv);
static int   inst_mount_cdrom         (int show_err);
static int   inst_mount_nfs           (void);
static int   inst_start_rescue        (void);
static int   add_instsys              (void);
static void  inst_yast_done           (void);
static int   inst_execute_yast        (void);
static int   inst_commit_install      (void);
static int   inst_choose_netsource    (void);
static int   inst_choose_netsource_cb (dia_item_t di);
#if defined(__s390__) || defined(__s390x__)
static int   inst_choose_display      (void);
static int   inst_choose_display_cb   (dia_item_t di);
#endif
static int   inst_choose_source       (void);
static int   inst_choose_source_cb    (dia_item_t di);
static int   inst_menu_cb             (dia_item_t di);
static int   inst_mount_smb           (void);
static int   inst_do_ftp              (void);
static int   inst_do_http             (void);
static int   inst_get_proxysetup      (void);
static int   inst_do_tftp             (void);
static int choose_dud(char **dev);
static void  inst_swapoff             (void);

static dia_item_t di_inst_menu_last = di_none;
static dia_item_t di_inst_choose_source_last = di_none;
static dia_item_t di_inst_choose_netsource_last = di_none;
#if defined(__s390__) || defined(__s390x__)  
static dia_item_t di_inst_choose_display_last = di_none;
#endif


int inst_menu()
{
  dia_item_t di;
  dia_item_t items[] = {
    di_inst_install,
    di_inst_system,
    di_inst_rescue,
    di_none
  };

  /* hope this is correct... */
  config.net.do_setup = 0;

  di = dia_menu2(txt_get(TXT_MENU_START), 40, inst_menu_cb, items, di_inst_menu_last);

  return di == di_none ? 1 : 0;
}


/*
 * return values:
 * -1    : abort (aka ESC)
 *  0    : ok
 *  other: stay in menu
 */
int inst_menu_cb(dia_item_t di)
{
  int error = 0;
  int rc = 1;

  di_inst_menu_last = di;

  switch(di) {
    case di_inst_install:
      config.rescue = 0;
      error = inst_start_install();
     /*
      * Back to main menu.
      */
      rc = -1;
      break;

    case di_inst_system:
      error = root_boot_system();
      break;

    case di_inst_rescue:
      config.rescue = 1;
      error = inst_start_rescue();
      break;

    default:
      break;
  }

  config.redraw_menu = 0;

  if(!error) rc = 0;

  return rc;
}


int inst_choose_netsource()
{
  dia_item_t di;
  dia_item_t items[] = {
    di_netsource_ftp,
    di_netsource_http,
    di_netsource_nfs,
    di_netsource_smb,
    di_netsource_tftp,
    di_none
  };

  inst_umount();

  if(!(config.test || config.net.cifs.binary)) items[3] = di_skip;

  if(di_inst_choose_netsource_last == di_none) {
    switch(config.instmode) {
      case inst_ftp:
        di_inst_choose_netsource_last = di_netsource_ftp;
        break;

      case inst_http:
        di_inst_choose_netsource_last = di_netsource_http;
        break;

      case inst_smb:
        di_inst_choose_netsource_last = di_netsource_smb;
        break;

      case inst_tftp:
        di_inst_choose_netsource_last = di_netsource_tftp;
        break;

      default:
        di_inst_choose_netsource_last = di_netsource_nfs;
        break;
    }
  }

  di = dia_menu2(txt_get(TXT_CHOOSE_NETSOURCE), 33, inst_choose_netsource_cb, items, di_inst_choose_netsource_last);

  return di == di_none ? -1 : 0;
}


/*
 * return values:
 * -1    : abort (aka ESC)
 *  0    : ok
 *  other: stay in menu
 */
int inst_choose_netsource_cb(dia_item_t di)
{
  int error = FALSE;

  di_inst_choose_netsource_last = di;

  switch(di) {
    case di_netsource_nfs:
      error = inst_mount_nfs();
      break;

    case di_netsource_smb:
      error = inst_mount_smb();
      break;

    case di_netsource_ftp:
      error = inst_do_ftp();
      break;

    case di_netsource_http:
      error = inst_do_http();
      break;

    case di_netsource_tftp:
      error = inst_do_tftp();
      break;

    default:
      break;
  }

  if(error) dia_message(txt_get(TXT_RI_NOT_FOUND), MSGTYPE_ERROR);

  return error ? 1 : 0;
}

#if defined(__s390__) || defined(__s390x__)  
int inst_choose_display()
{
  dia_item_t di;
  dia_item_t items[] = {
    di_display_x11,
    di_display_vnc,
    di_display_ssh,
    di_none
  };

  di = dia_menu2(txt_get(TXT_CHOOSE_DISPLAY), 33, inst_choose_display_cb, items, di_inst_choose_display_last);

  return di == di_none ? -1 : 0;
}


/*
 * return values:
 * -1    : abort (aka ESC)
 *  0    : ok
 *  other: stay in menu
 */
int inst_choose_display_cb(dia_item_t di)
{
  int rc;
  di_inst_choose_display_last = di;

  switch(di) {
    case di_display_x11:
      if((rc = net_get_address(txt_get(TXT_XSERVER_IP), &config.net.displayip, 1)))
        return rc;
      break;

    case di_display_vnc:
      config.vnc=1;
      net_ask_password();
      break;

    case di_display_ssh:
      config.usessh=1;
      net_ask_password();
      break;

    default:
      break;
  }

  return 0;
}
#endif


/*
 * Ask for repo location.
 *
 * return:
 *   0: ok
 *   1: abort
 */
int inst_choose_source()
{
  dia_item_t di;
  dia_item_t items[] = {
    di_source_cdrom,
    di_source_net,
    di_source_hd,
    di_none
  };

  if(di_inst_choose_source_last == di_none) {
    di_inst_choose_source_last = di_source_cdrom;
    if(config.url.install) {
      if(config.url.install->is.network) {
        di_inst_choose_source_last = di_source_net;
      }
      else if(!config.url.install->is.cdrom) {
        di_inst_choose_source_last = di_source_hd;
      }
    }
  }

  di = dia_menu2(txt_get(TXT_CHOOSE_SOURCE), 33, inst_choose_source_cb, items, di_inst_choose_source_last);

  return di == di_none ? 1 : 0;
}


/*
 * Repo location menu.
 *
 * return values:
 * -1    : abort (aka ESC)
 *  0    : ok
 *  other: stay in menu
 */
int inst_choose_source_cb(dia_item_t di)
{
  int err = 0;
  char tmp[200];

  di_inst_choose_source_last = di;

  switch(di) {
    case di_source_cdrom:
      str_copy(&config.serverdir, NULL);
      err = inst_mount_cdrom(0);
      if(err) {
        sprintf(tmp, txt_get(TXT_INSERT_CD), 1);
        dia_message(tmp, MSGTYPE_INFOENTER);
        err = inst_mount_cdrom(1);
      }
      break;

    case di_source_net:
      err = inst_choose_netsource();
      break;

    case di_source_hd:
      err = inst_mount_harddisk();
      break;

    default:
      break;
  }

  if(err) dia_message(txt_get(TXT_RI_NOT_FOUND), MSGTYPE_ERROR);

  return err ? 1 : 0;
}


int inst_try_cdrom(char *dev)
{

  return 0;
}


int inst_mount_cdrom(int show_err)
{
  int rc;
  slist_t *sl;
  window_t win;

  if(config.instmode_extra == inst_cdwithnet) {
    rc = net_config();
  }
  else {
    set_instmode(inst_cdrom);

    if(config.net.do_setup && (rc = net_config())) return rc;
  }

  dia_info(&win, txt_get(TXT_TRY_CD_MOUNT));

  rc = 1;

  if(config.cdrom) rc = inst_try_cdrom(config.cdrom);

  if(rc) {
    if(config.cdromdev) rc = inst_try_cdrom(config.cdromdev);
    if(rc) {
      for(sl = config.cdroms; sl; sl = sl->next) {
        if(!(rc = inst_try_cdrom(sl->key))) {
          str_copy(&config.cdrom, sl->key);
          break;
        }
      }
    }
  }

  win_close(&win);

  if(rc) {
    if(show_err) {
      dia_message(txt_get(rc == 2 ? TXT_RI_NOT_FOUND : TXT_ERROR_CD_MOUNT), MSGTYPE_ERROR);
     }
  }

  return rc;
}


/*
 * build a partition list
 */
int inst_choose_partition(char **partition, int swap, char *txt_menu, char *txt_input)
{
  int i, j, rc, item_cnt, item_cnt1, item_cnt2, item_cnt3;
  char **items, **values;
  char **items1, **values1;
  char **items2, **values2;
  char **items3, **values3;
  char *type;
  slist_t *sl;
  char buf[256], *dev;
  int found = 0, item_mk_part = 0, item_mk_file = 0;
  char *s, *tmp = NULL;
  static char *last_part = NULL;
  int last_item, last_found, last_item1 = 0, last_item2 = 0, last_item3 = 0;
  char *module;

  util_update_disk_list(NULL, 1);
  util_update_swap_list();

  for(i = 0, sl = config.partitions; sl; sl = sl->next) i++;

  /*
   * Just max values, actual lists might be shorter.
   * list1: swap, list2: with fs or empty, list3: with fs
   */
  items1 = calloc(i + 4, sizeof *items1);
  values1 = calloc(i + 4, sizeof *values1);
  items2 = calloc(i + 4, sizeof *items2);
  values2 = calloc(i + 4, sizeof *values2);
  items3 = calloc(i + 4, sizeof *items3);
  values3 = calloc(i + 4, sizeof *values3);

  for(item_cnt1 = item_cnt2 = item_cnt3 = 0, sl = config.partitions; sl; sl = sl->next) {
    if(
      sl->key && !slist_getentry(config.swaps, sl->key)		/* don't show active swaps */
    ) {
      if(blk_size(long_dev(sl->key)) < (128 << 10)) continue;

      if(*partition && !strcmp(sl->key, *partition)) found = 1;
      last_found = last_part && !strcmp(sl->key, last_part) ? 1 : 0;

      sprintf(buf, "%s (%s)", sl->key, blk_ident(long_dev(sl->key)));

      type = fstype(long_dev(sl->key));

      if(type && !strcmp(type, "swap")) {
        values1[item_cnt1] = strdup(sl->key);
        items1[item_cnt1++] = strdup(buf);
        if(last_found) last_item1 = item_cnt1;
      }
      else if(type || swap) {
        values2[item_cnt2] = strdup(sl->key);
        items2[item_cnt2++] = strdup(buf);
        if(last_found) last_item2 = item_cnt2;
        if(type) {
          values3[item_cnt3] = strdup(sl->key);
          items3[item_cnt3++] = strdup(buf);
          if(last_found) last_item3 = item_cnt3;
        }
      }
    }
  }

  if(*partition && !found) {
    sprintf(buf, "%s (%s)", *partition, blk_ident(long_dev(*partition)));
    values2[item_cnt2] = strdup(*partition);
    items2[item_cnt2++] = strdup(buf);
  }

  if(swap) {
    values1[item_cnt1] = NULL;
    items1[item_cnt1++] = strdup("create swap partition");
    item_mk_part = item_cnt1;
    if(config.swap_file_size) {
      values1[item_cnt1] = NULL;
      items1[item_cnt1++] = strdup("create swap file");
      item_mk_file = item_cnt1;
    }
  }

  if(swap) {
    item_cnt = item_cnt1;
    items = items1;
    values = values1;
    last_item = last_item1;
  }
  else {
    item_cnt = item_cnt3;
    items = items3;
    values = values3;
    last_item = last_item3;
  }

  rc = 1;
  if(item_cnt) {
    i = dia_list(txt_menu, 36, NULL, items, last_item, align_left);

    if(i == 0) rc = -1;

    if(i > 0 && values[i - 1]) {
      str_copy(&last_part, values[i - 1]);
      str_copy(partition, values[i - 1]);
      rc = 0;
    }

    if(i == item_mk_part) {
      do {
        i = dia_list("create a swap partition", 36, NULL, items2, last_item2, align_left);
        if(i > 0 && values2[i - 1]) {
          str_copy(&last_part, values2[i - 1]);
          dev = long_dev(values2[i - 1]);
          sprintf(buf, "Warning: all data on %s will be deleted!", dev);
          j = dia_contabort(buf, NO);
          if(j == YES) {
            sprintf(buf, "/sbin/mkswap %s >/dev/null 2>&1", dev);
            fprintf(stderr, "mkswap %s\n", dev);
            if(!system(buf)) {
              fprintf(stderr, "swapon %s\n", dev);
              if(swapon(dev, 0)) {
                fprintf(stderr, "swapon: ");
                perror(dev);
                dia_message(txt_get(TXT_ERROR_SWAP), MSGTYPE_ERROR);
              }
              else {
                rc = 0;
              }
            }
            else {
              dia_message("mkswap failed", MSGTYPE_ERROR);
            }
          }
          else {
            rc = 1;
          }
        }
      }
      while(rc && i);
    }
    else if(i == item_mk_file) {
      do {
        i = dia_list("select partition for swap file", 36, NULL, items3, last_item3, align_left);
        if(i > 0 && values3[i - 1]) {
          str_copy(&last_part, values3[i - 1]);
          dev = long_dev(values3[i - 1]);
          util_fstype(dev, &module);
          if(module) mod_modprobe(module, NULL);
          j = util_mount_rw(dev, config.mountpoint.swap);
          if(j) {
            dia_message("mount failed", MSGTYPE_ERROR);
          }
          else {
            char *tmp, file[256];
            int fd;
            window_t win;
            unsigned swap_size = config.swap_file_size << (20 - 18);	/* in 256k chunks */

            sprintf(file, "%s/suseswap.img", config.mountpoint.swap);

            tmp = calloc(1, 1 << 18);

            fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(fd >= 0) {
              sprintf(buf, "creating swap file 'suseswap.img' (%u MB)", config.swap_file_size);
              dia_status_on(&win, buf);
              for(j = 0; j < swap_size; j++) {
                if(write(fd, tmp, 1 << 18) != 1 << 18) break;
                fsync(fd);
                dia_status(&win, (j + 1) * 100 / swap_size);
              }
              close(fd);
              dia_status_off(&win);
            }
            free(tmp);

            if(j != swap_size) {
              dia_message("failed to create swapfile", MSGTYPE_ERROR);
            }
            else {
              sprintf(buf, "/sbin/mkswap %s >/dev/null 2>&1", file);
              fprintf(stderr, "mkswap %s\n", file);
              if(!system(buf)) {
                fprintf(stderr, "swapon %s\n", file);
                if(swapon(file, 0)) {
                  fprintf(stderr, "swapon: ");
                  perror(file);
                  dia_message(txt_get(TXT_ERROR_SWAP), MSGTYPE_ERROR);
                }
                else {
                  umount2(config.mountpoint.swap, MNT_DETACH);
                  rc = 0;
                }
              }
              else {
                dia_message("mkswap failed", MSGTYPE_ERROR);
              }
            }
            if(rc) util_umount(config.mountpoint.swap);
          }
        }
      }
      while(rc && i);
    }
  }
  else {
    str_copy(&tmp, *partition);
    rc = dia_input2(txt_input, &tmp, 30, 0);
    if(!rc) {
      s = tmp;
      if(tmp && strstr(tmp, "/dev/") == tmp) s = tmp  + sizeof "/dev/" - 1;
      str_copy(partition, s);
      str_copy(&tmp, NULL);
    }
  }

  for(i = 0; i < item_cnt1; i++) { free(items1[i]); free(values1[i]); }
  free(items1);
  free(values1);
  for(i = 0; i < item_cnt2; i++) { free(items2[i]); free(values2[i]); }
  free(items2);
  free(values2);
  for(i = 0; i < item_cnt3; i++) { free(items3[i]); free(values3[i]); }
  free(items3);
  free(values3);

  // fprintf(stderr, "rc = %d\n", rc);

  return rc;
}


/*
 * Select and mount disk repo.
 *
 * return:
 *   0: ok
 *   1: failed
 */
int inst_mount_harddisk()
{
  int err = 0;
  char *device = NULL, *path = NULL;

  if(config.net.do_setup && net_config()) return 1;

  if(
    config.url.install &&
    (
      config.url.install->scheme == inst_disk ||
      config.url.install->scheme == inst_hd
    )
  ) {
    str_copy(&device, config.url.install->device);
    str_copy(&path, config.url.install->path);
  }

  do {
    if(inst_choose_partition(&device, 0, txt_get(TXT_CHOOSE_PARTITION), txt_get(TXT_ENTER_PARTITION))) err = 1;
    if(!err && dia_input2(txt_get(TXT_ENTER_HD_DIR), &path, 30, 0)) err = 1;

    if(err) break;

    url_free(config.url.install);
    config.url.install = url_set("hd:");
    str_copy(&config.url.install->device, device);
    str_copy(&config.url.install->path, path);
    str_copy(&config.url.install->used.device, long_dev(device));

    err = auto2_find_repo() ? 0 : 1;
  } while(err);

  str_copy(&device, NULL);
  str_copy(&path, NULL);

  return err;
}


int inst_mount_nfs()
{
  int rc;
  char text[256];

  set_instmode(inst_nfs);

  if((rc = net_config())) return rc;

  if(config.win) {	/* ###### check really needed? */
    sprintf(text, txt_get(TXT_INPUT_NETSERVER), get_instmode_name_up(config.instmode));
    if((rc = net_get_address(text, &config.net.server, 1))) return rc;
    if((rc = dia_input2(txt_get(TXT_INPUT_DIR), &config.serverdir, 30, 0))) return rc;
  }
  return do_mount_nfs();
}


int inst_mount_smb()
{
  int rc;
  char buf[256];

  set_instmode(inst_smb);

  if((rc = net_config())) return rc;

  if(config.win) {	/* ###### check really needed? */
    sprintf(buf, txt_get(TXT_INPUT_NETSERVER), get_instmode_name_up(config.instmode));
    if((rc = net_get_address(buf, &config.net.server, 1))) return rc;
    if((rc = dia_input2(txt_get(TXT_SMB_ENTER_SHARE), &config.net.share, 30, 0))) return rc;
    if((rc = dia_input2(txt_get(TXT_INPUT_DIR), &config.serverdir, 30, 0))) return rc;
  }

  rc = dia_yesno(txt_get(TXT_SMB_GUEST_LOGIN), YES);

  if(rc == ESCAPE) {
    return -1;
  }
  else {
    if(rc == YES) {
      str_copy(&config.net.user, NULL);
      str_copy(&config.net.password, NULL);
      str_copy(&config.net.workgroup, NULL);
    }
    else {
      if((rc = dia_input2(txt_get(TXT_SMB_ENTER_USER), &config.net.user, 20, 0))) return rc;
      if((rc = dia_input2(txt_get(TXT_SMB_ENTER_PASSWORD), &config.net.password, 20, 1))) return rc;
      if((rc = dia_input2(txt_get(TXT_SMB_ENTER_WORKGROUP), &config.net.workgroup, 20, 0))) return rc;
    }
  }

  return do_mount_smb();
}


/*
 * Start YaST.
 *
 * return:
 *   0: ok
 *   1: err
 */
int inst_start_install()
{
  int err = 0;

  util_splash_bar(60, SPLASH_60);

  if(config.manual) {
    util_umount_all();
    util_clear_downloads();

    url_free(config.url.instsys);
    config.url.instsys = url_set(config.rescue ? config.rescueimage : config.rootimage);

    if(inst_choose_source()) return 1;
  }

  LXRC_WAIT
  
  if(config.rescue) {
    /* get rid of repo */
    url_umount(config.url.install);

    return 0;
  }

#if defined(__s390__) || defined(__s390x__)
  if(
    (config.net.setup & NS_DISPLAY) &&
    inst_choose_display()
  ) return 1;
#endif
  
  LXRC_WAIT

  err = inst_execute_yast();

  util_umount_all();
  util_clear_downloads();

  if(!err) {
    err = inst_commit_install();
    if(err) {
      config.rescue = 0;
      config.manual |= 1;
      util_disp_init();
    }
  }

  return err;
}


/* we might as well just use inst_start_install() instead... */
int inst_start_rescue()
{
  int rc;

  if((rc = inst_choose_source())) return rc;

  config.inst_ramdisk = load_image(inst_rootimage_tm, config.instmode, txt_get(TXT_LOADING_RESCUE));

  inst_umount();

  if(config.inst_ramdisk >= 0) {
    root_set_root(config.ramdisk[config.inst_ramdisk].dev);
  }

  util_debugwait("rescue system loaded");

  return config.inst_ramdisk < 0 ? -1 : 0;
}


/*
 * Prepare instsys.
 *
 * return:
 *   0: ok
 *   1: error
 */
int add_instsys()
{
  char buf[256];
  int err = 0;
  char *argv[3] = { };

  if(!config.url.instsys->mount) return 1;

  setenv("INSTSYS", config.url.instsys->mount, 1);

  setenv("TERM", config.term ?: config.serial ? "screen" : "linux", 1);

  setenv("ESCDELAY", config.serial ? "1100" : "10", 1);

  setenv("YAST_DEBUG", "/debug/yast.debug", 1);

  sprintf(buf, "file:%s/.instsys.config", config.url.instsys->mount);
  file_read_info_file(buf, kf_cfg);

  file_write_install_inf("");

  if(
    config.instsys_complain &&
    config.initrd_id &&
    config.instsys_id &&
    strcmp(config.initrd_id, config.instsys_id)
  ) {
    int win;

    if(!(win = config.win)) util_disp_init();
    if(config.instsys_complain == 1) {
      dia_message(
        "Installation system does not match your boot medium.\n\n"
        "It may make your bugreports worthless.",
        MSGTYPE_ERROR
      );
    }
    else {
      dia_message(
        "Installation system does not match your boot medium.\n\n"
        "Sorry, this will not work.",
        MSGTYPE_ERROR
      );
      err = 1;
    }
    if(!win) util_disp_done();
  }

  if(
    config.update_complain &&
    config.update.expected_name_list
  ) {
    int win;
    slist_t *sl;

    for(sl = config.update.expected_name_list; sl; sl = sl->next) {
      if(!slist_getentry(config.update.name_list, sl->key)) break;
    }

    if(sl) {
      if(!(win = config.win)) util_disp_init();

      sprintf(buf,
        "The following driver update has not been applied:\n\n%s\n\n"
        "You can continue, but things will not work as expected.\n"
        "If you don't want to see this message, boot with 'updatecomplain=0'.",
        sl->key
      );

      dia_message(buf, MSGTYPE_ERROR);

      if(!win) util_disp_done();
    }
  }

  if(!config.test) {
    // fake mtab
    system("rm /etc/mtab 2>/dev/null; cat /proc/mounts >/etc/mtab");

    argv[1] = config.url.instsys->mount;
    argv[2] = "/";
    util_lndir_main(3, argv);
  }

  return err;
}


void inst_yast_done()
{
  int count;
  char *buf = NULL;

  if(config.test) return;

  lxrc_set_modprobe("/etc/nothing");

  lxrc_killall(0);

  for(count = 0; count < 8; count++) {
    strprintf(&buf, "/dev/loop%d", count);
    util_detach_loop(buf);
  }

  str_copy(&buf, NULL);
}


int inst_execute_yast()
{
  int i, rc;
  char *setupcmd = NULL;
  FILE *f;

  rc = add_instsys();
  if(rc) {
    inst_yast_done();
    return rc;
  }

  if(!config.test) {
    lxrc_set_modprobe("/sbin/modprobe");
    if(util_check_exist("/sbin/update")) system("/sbin/update");
  }

  i = 0;
  util_free_mem();
  if(config.addswap) {
    i = ask_for_swap(
      config.addswap == 2 ? -1 : config.memory.min_yast - config.memory.min_free,
      txt_get(TXT_LOW_MEMORY2)
    );
  }

  if(i == -1) {
    inst_yast_done();
    return -1;
  }

  util_free_mem();

  if(!config.test && config.usessh && config.net.sshpassword) {
    if((f = popen("/usr/sbin/chpasswd", "w"))) {
      fprintf(f, "root:%s\n", config.net.sshpassword);
      pclose(f);
    }
  }

  /* start shells only _after_ the swap dialog */
  if(!config.test && !config.noshell) {
    util_start_shell("/dev/tty2", "/bin/bash", 3);
    util_start_shell("/dev/tty5", "/bin/bash", 3);
    util_start_shell("/dev/tty6", "/bin/bash", 3);
  }

  disp_set_color(COL_WHITE, COL_BLACK);
  if(config.win) util_disp_done();

  if(config.splash && config.textmode) system("echo 0 >/proc/splash");

  str_copy(&setupcmd, config.setupcmd);

  if(config.url.install->scheme == inst_exec) {
    strprintf(&setupcmd, "setctsid `showconsole` %s",
      *config.url.install->path ? config.url.install->path : "/bin/sh"
    );
  }

  fprintf(stderr, "starting %s\n", setupcmd);

  LXRC_WAIT

  kbd_end(1);
  util_notty();

  if(config.test) {
    rc = system("/bin/bash 2>&1");
  }
  else {
    if(config.zombies) {
      rc = system(setupcmd);
    }
    else {
      pid_t pid, inst_pid;

      inst_pid = fork();

      if(inst_pid) {
        // fprintf(stderr, "%d: inst_pid = %d\n", getpid(), inst_pid);

        while((pid = waitpid(-1, &rc, 0))) {
          // fprintf(stderr, "%d: chld(%d) = %d\n", getpid(), pid, rc);
          if(pid == inst_pid) {
            // fprintf(stderr, "%d: last chld\n", getpid());
            break;
          }
        }

        // fprintf(stderr, "%d: back from loop\n", getpid());
      }
      else {
        // fprintf(stderr, "%d: system()\n", getpid());
        rc = system(setupcmd);
        // fprintf(stderr, "%d: exit(%d)\n", getpid(), rc);
        exit(WEXITSTATUS(rc));
      }

      // fprintf(stderr, "%d: back, rc = %d\n", getpid(), rc);
    }
  }

  if(rc) {
    if(rc == -1) {
      rc = errno;
    }
    else if(WIFEXITED(rc)) {
      rc = WEXITSTATUS(rc);
    }
  }

  if(!config.listen) {
    freopen(config.console, "r", stdin);
    freopen(config.console, "a", stdout);
    freopen(config.stderr_name, "a", stderr);
  }
  else {
    dup2(1, 0);
    config.kbd_fd = 0;
  }
  kbd_init(0);
  util_notty();

  str_copy(&setupcmd, NULL);

  if(config.splash && config.textmode) system("echo 1 >/proc/splash");

  fprintf(stderr, "install program exit code is %d\n", rc);

  /* Redraw erverything and go back to the main menu. */
  config.redraw_menu = 1;

  fprintf(stderr, "sync...");
  sync();
  fprintf(stderr, " ok\n");

  util_debugwait("going to read yast.inf");

  i = file_read_yast_inf();
  if(!rc) rc = i;

  disp_cursor_off();
  kbd_reset();

  if(rc || config.aborted) {
    config.rescue = 0;
    config.manual |= 1;
  }

  if(config.manual) util_disp_init();

  if(rc && config.win) {
    dia_message(txt_get(TXT_ERROR_INSTALL), MSGTYPE_ERROR);
  }

  if(!config.test) {
    /* never trust yast */
    mount(0, "/", 0, MS_MGC_VAL | MS_REMOUNT, 0);
  }

  /* turn off swap */
  inst_swapoff();

  inst_yast_done();

  if(config.aborted) {
    config.aborted = 0;
    rc = -1;
  }

  return rc;
}


/*
 * If we should reboot, do it.
 *
 * return:
 *   0: ok (only in test mode, obviously)
 *   1: failed
 */
int inst_commit_install()
{
  int err = 0;

  if(reboot_ig == 2) {
    reboot(RB_POWER_OFF);
  }
  else if(reboot_ig) {

    if(config.rebootmsg) {
      disp_clear_screen();
      util_disp_init();
      dia_message(txt_get(TXT_DO_REBOOT), MSGTYPE_INFO);
    }

    if(config.test) {
      fprintf(stderr, "*** reboot ***\n");
    }
    else {
#if	defined(__s390__) || defined(__s390x__)
      reboot(RB_POWER_OFF);
#else
      reboot(RB_AUTOBOOT);
#endif
    }
    err = 1;
  }

  return err;
}


int inst_umount()
{
  int i = 0, j;

  j = util_umount(config.mountpoint.instsys);
  if(j == EBUSY) i = EBUSY;

  j = util_umount(config.mountpoint.instdata);
  if(j == EBUSY) i = EBUSY;

  if(config.extramount) {
    j = util_umount(config.mountpoint.extra);
    if(j == EBUSY) i = EBUSY;
    config.extramount = 0;
  }

  return i;
}


int inst_do_ftp()
{
  int rc;
  window_t win;
  char buf[256];

  set_instmode(inst_ftp);

  if((rc = net_config())) return rc;

  do {
    sprintf(buf, txt_get(TXT_INPUT_NETSERVER), get_instmode_name_up(config.instmode));
    if((rc = net_get_address(buf, &config.net.server, 1))) return rc;
    if((rc = inst_get_proxysetup())) return rc;

    sprintf(buf, txt_get(TXT_TRY_REACH_SERVER), get_instmode_name_up(config.instmode));
    dia_info(&win, buf);
    rc = net_open(NULL);
    win_close(&win);

    if(rc < 0) {
      util_print_net_error();
    }
    else {
      rc = 0;
    }
  }
  while(rc);

  if(dia_input2(txt_get(TXT_INPUT_DIR), &config.serverdir, 30, 0)) return -1;
  util_truncate_dir(config.serverdir);

  return 0;
}


int inst_do_http()
{
  int rc;
  window_t win;
  char buf[256];

  set_instmode(inst_http);

  if((rc = net_config())) return rc;

  do {
    sprintf(buf, txt_get(TXT_INPUT_NETSERVER), get_instmode_name_up(config.instmode));
    if((rc = net_get_address(buf, &config.net.server, 1))) return rc;
    if((rc = inst_get_proxysetup())) return rc;

    sprintf(buf, txt_get(TXT_TRY_REACH_SERVER), get_instmode_name_up(config.instmode));
    dia_info(&win, buf);
    rc = net_open(NULL);
    win_close(&win);

    if(rc < 0) {
      util_print_net_error();
    }
    else {
      rc = 0;
    }
  }
  while(rc);

  if(dia_input2(txt_get(TXT_INPUT_DIR), &config.serverdir, 30, 0)) return -1;
  util_truncate_dir(config.serverdir);

  return 0;
}


int inst_get_proxysetup()
{
  int rc;
  char *s, tmp[256], buf[256];
  unsigned u;

  if(config.instmode == inst_ftp) {
    if(config.instmode == inst_ftp) {
      strcpy(buf, txt_get(TXT_ANONYM_FTP));
    }
    else {
      sprintf(buf,
        "Do you need a username and password to access the %s server?",
        get_instmode_name_up(config.instmode)
      );
    }
    rc = dia_yesno(buf, NO);
    if(rc == ESCAPE) return -1;

    if(rc == NO) {
      str_copy(&config.net.user, NULL);
      str_copy(&config.net.password, NULL);
    }
    else {
      sprintf(buf, txt_get(TXT_ENTER_USER), get_instmode_name_up(config.instmode));
      if((rc = dia_input2(buf, &config.net.user, 20, 0))) return rc;
      sprintf(buf, txt_get(TXT_ENTER_PASSWORD), get_instmode_name_up(config.instmode));
      if((rc = dia_input2(buf, &config.net.password, 20, 1))) return rc;
    }
  }

  sprintf(buf, txt_get(TXT_WANT_PROXY), get_instmode_name_up(config.net.proxyproto));
  rc = dia_yesno(buf, NO);
  if(rc == ESCAPE) return -1;

  if(rc == YES) {
    sprintf(buf, txt_get(TXT_ENTER_PROXY), get_instmode_name_up(config.net.proxyproto));
    if((rc = net_get_address(buf, &config.net.proxy, 1))) return rc;

    *tmp = 0;
    if(config.net.proxyport) sprintf(tmp, "%u", config.net.proxyport);

    do {
      sprintf(buf, txt_get(TXT_ENTER_PROXYPORT), get_instmode_name_up(config.net.proxyproto));
      rc = dia_input(buf, tmp, 6, 6, 0);
      if(rc) return rc;
      u = strtoul(tmp, &s, 0);
      if(*s) {
        rc = -1;
        dia_message(txt_get(TXT_INVALID_INPUT), MSGTYPE_ERROR);
      }
    }
    while(rc);

    config.net.proxyport = u;
  }
  else {
    name2inet(&config.net.proxy, "");
    config.net.proxyport = 0;
  }

  return 0;
}


int inst_do_tftp()
{
  int rc;
  window_t win;
  char buf[256];

  set_instmode(inst_tftp);

  config.net.proxyport = 0;

  if((rc = net_config())) return rc;

  do {
    sprintf(buf, txt_get(TXT_INPUT_NETSERVER), get_instmode_name_up(config.instmode));
    if((rc = net_get_address(buf, &config.net.server, 1))) return rc;

    sprintf(buf, txt_get(TXT_TRY_REACH_SERVER), get_instmode_name_up(config.instmode));
    dia_info(&win, buf);
    rc = net_open(NULL);
    win_close(&win);

    if(rc < 0) {
      util_print_net_error();
    }
    else {
      rc = 0;
    }
  }
  while(rc);

  if(dia_input2(txt_get(TXT_INPUT_DIR), &config.serverdir, 30, 0)) return -1;
  util_truncate_dir(config.serverdir);

  return 0;
}


/*
 * Ask for and apply driver update.
 *
 * return values:
 *  0    : ok
 *  1    : abort
 */
int inst_update_cd()
{
  int i, update_rd;
  char *dev, *buf = NULL, *argv[3], *module;
  unsigned old_count;
  slist_t **names;
  window_t win;

  config.update.shown = 1;

  if(choose_dud(&dev)) return 1;

  if(!dev) return 0;

  util_fstype(long_dev(dev), &module);
  if(module) mod_modprobe(module, NULL);

  /* ok, mount it */
  i = util_mount_ro(long_dev(dev), config.mountpoint.update);

  if(i) {
    dia_message(txt_get(TXT_DUD_FAIL), MSGTYPE_ERROR);
    return 0;
  }

  old_count = config.update.count;

  /* point at list end */
  for(names = &config.update.name_list; *names; names = &(*names)->next);

  dia_info(&win, txt_get(TXT_DUD_READ));

  strprintf(&buf, "%s/%s", config.mountpoint.update, SP_FILE);

  if(util_check_exist(buf) == 'r' && !util_check_exist("/" SP_FILE)) {
    argv[1] = buf;
    argv[2] = "/";
    util_cp_main(3, argv);
  }

  util_chk_driver_update(config.mountpoint.update, dev);

  strprintf(&buf, "%s/driverupdate", config.mountpoint.update);
  if(util_check_exist(buf) == 'r') {
    update_rd = load_image(buf, inst_file, txt_get(TXT_LOADING_UPDATE));

    if(update_rd >= 0) {
      i = ramdisk_mount(update_rd, config.mountpoint.update);
      if(!i) util_chk_driver_update(config.mountpoint.update, get_instmode_name(inst_file));
      ramdisk_free(update_rd);
    }
  }

  util_umount(config.mountpoint.update);

  util_do_driver_updates();

  win_close(&win);

  if(old_count == config.update.count) {
    dia_message(txt_get(TXT_DUD_NOTFOUND), MSGTYPE_INFO);
  }
  else {
    if(*names) {
      dia_show_lines2(txt_get(TXT_DUD_ADDED), *names, 64);
    }
    else {
      dia_message(txt_get(TXT_DUD_OK), MSGTYPE_INFO);
    }
  }

  free(buf);

  return 0;
}


/*
 * Let user enter a device for driver updates
 * (*dev = NULL if she changed her mind).
 *
 * return values:
 *  0    : ok
 *  1    : abort
 */
int choose_dud(char **dev)
{
  int i, j, item_cnt, last_item, dev_len, item_width;
  int sort_cnt, err = 0;
  char *s, *s1, *s2, *s3, *buf = NULL, **items, **values;
  hd_data_t *hd_data;
  hd_t *hd, *hd1;
  window_t win;

  *dev = NULL;

  hd_data = calloc(1, sizeof *hd_data);

  if(config.manual < 2) {
    dia_info(&win, "Searching for storage devices...");
    hd_list(hd_data, hw_block, 1, NULL);
    win_close(&win);
  }

  for(i = 0, hd = hd_data->hd; hd; hd = hd->next) {
    if(!hd_is_hw_class(hd, hw_block)) continue;

    /* don't look at whole disk devs, if there are partitions */
    if(
      (hd1 = hd_get_device_by_idx(hd_data, hd->attached_to)) &&
      hd1->base_class.id == bc_storage_device
    ) {
      hd1->status.available = status_no;
    }

    i++;
  }

  /* just max values, actual lists might be shorter */
  items = calloc(i + 1+ 2, sizeof *items);
  values = calloc(i + 1 + 2, sizeof *values);

  item_cnt = 0;

  /* max device name length */
  for(dev_len = 0, hd = hd_data->hd; hd; hd = hd->next) {
    if(
      !hd_is_hw_class(hd, hw_block) ||
      hd->status.available == status_no ||
      !hd->unix_dev_name
    ) continue;

    j = strlen(hd->unix_dev_name);
    if(j > dev_len) dev_len = j;
  }
  dev_len = dev_len > 5 ? dev_len - 5 : 1;

  item_width = sizeof "other device" - 1;

  for(sort_cnt = 0; sort_cnt < 4; sort_cnt++) {
    for(hd = hd_data->hd; hd; hd = hd->next) {
      if(
        !hd_is_hw_class(hd, hw_block) ||
        hd->status.available == status_no ||
        !hd->unix_dev_name ||
        strncmp(hd->unix_dev_name, "/dev/", sizeof "/dev/" - 1)
      ) continue;

      j = 0;
      switch(sort_cnt) {
        case 0:
          if(hd_is_hw_class(hd, hw_floppy)) j = 1;
          break;

        case 1:
          if(hd_is_hw_class(hd, hw_cdrom)) j = 1;
          break;

        case 2:
          if(hd_is_hw_class(hd, hw_usb)) {
            j = 1;
          }
          else {
            hd1 = hd_get_device_by_idx(hd_data, hd->attached_to);
            if(hd1 && hd_is_hw_class(hd1, hw_usb)) j = 1;
          }
          break;

        default:
          j = 1;
          break;
      }

      if(!j) continue;

      hd->status.available = status_no;

      if(
        !(hd1 = hd_get_device_by_idx(hd_data, hd->attached_to)) ||
        hd1->base_class.id != bc_storage_device
      ) {
        hd1 = hd;
      }
      
      s1 = hd1->model;
      if(hd_is_hw_class(hd, hw_floppy)) s1 = "";

      s2 = "Disk";
      if(hd_is_hw_class(hd, hw_partition)) s2 = "Partition";
      if(hd_is_hw_class(hd, hw_floppy)) s2 = "Floppy";
      if(hd_is_hw_class(hd, hw_cdrom)) s2 = "CD-ROM";

      s3 = "";
      if(hd_is_hw_class(hd1, hw_usb)) s3 = "USB ";

      s = NULL;
      strprintf(&s, "%*s: %s%s%s%s",
        dev_len,
        short_dev(hd->unix_dev_name),
        s3,
        s2,
        *s1 ? ", " : "",
        s1
      );

      j = strlen(s);
      if(j > item_width) item_width = j;

      // fprintf(stderr, "<%s>\n", s);

      values[item_cnt] = strdup(short_dev(hd->unix_dev_name));
      items[item_cnt++] = s;
      s = NULL;
    }
  }

  last_item = 0;

  if(config.update.dev) {
    for(i = 0; i < item_cnt; i++) {
      if(values[i] && !strcmp(values[i], config.update.dev)) {
        last_item = i + 1;
        break;
      }
    }

    if(!last_item) {
      values[item_cnt] = strdup(config.update.dev);
      items[item_cnt++] = strdup(config.update.dev);
      last_item = item_cnt;
    }
  }

  values[item_cnt] = NULL;
  items[item_cnt++] = strdup("other device");

  if(item_width > 60) item_width = 60;

  if(item_cnt > 1) {
    i = dia_list(txt_get(TXT_DUD_SELECT), item_width + 2, NULL, items, last_item, align_left);
  }
  else {
    i = item_cnt;
  }

  if(i > 0) {
    s = values[i - 1];
    if(s) {
      str_copy(&config.update.dev, values[i - 1]);
      *dev = config.update.dev;
    }
    else {
      str_copy(&buf, NULL);
      i = dia_input2(txt_get(TXT_DUD_DEVICE), &buf, 30, 0);
      if(!i) {
        if(util_check_exist(long_dev(buf)) == 'b') {
          str_copy(&config.update.dev, short_dev(buf));
          *dev = config.update.dev;
        }
        else {
          dia_message(txt_get(TXT_DUD_INVALID_DEVICE), MSGTYPE_ERROR);
        }
      }
      else {
        err = 1;
      }
    }
  }
  else {
    err = 1;
  }

  for(i = 0; i < item_cnt; i++) { free(items[i]); free(values[i]); }
  free(items);
  free(values);

  free(buf);

  hd_free_hd_data(hd_data);

  free(hd_data);

  // fprintf(stderr, "dud dev = %s\n", *dev);

  return err;
}


void inst_swapoff()
{
  slist_t *sl;
  char buf[64];

  util_update_swap_list();

  if(config.test) return;

  for(sl = config.swaps; sl; sl = sl->next) {
    sprintf(buf, "/dev/%s", sl->key);
    fprintf(stderr, "swapoff %s\n", buf);
    swapoff(buf);
  }
}


/*
 * Mount nfs install source.
 *
 * If config.serverdir points to a file, mount one level higher and
 * loop-mount the file.
 */
int do_mount_nfs()
{
  int rc, file_type = 0;
  window_t win;
  char *buf = NULL, *serverdir = NULL, *file = NULL;
  char *path;

  str_copy(&config.serverpath, NULL);
  str_copy(&config.serverfile, NULL);

  path = config.serverdir && *config.serverdir ? config.serverdir : "/";

  strprintf(&buf,
    config.win ? txt_get(TXT_TRY_NFS_MOUNT) : "nfs: trying to mount %s:%s\n" ,
    config.net.server.name, path
  );

  if(config.win) {
    dia_info(&win, buf);
  }
  else {
    fprintf(stderr, "%s", buf);
  }

  fprintf(stderr, "Starting portmap.\n");
  system("portmap");

  rc = net_mount_nfs(config.mountpoint.instdata, &config.net.server, path);

  if(config.debug) fprintf(stderr, "nfs: err #1 = %d\n", rc);

  if(rc == ENOTDIR) {
    str_copy(&serverdir, path);

    if((file = strrchr(serverdir, '/')) && file != serverdir && file[1]) {
      *file++ = 0;
      mkdir(config.mountpoint.extra, 0755);

      fprintf(stderr, "nfs: trying to mount %s:%s\n", config.net.server.name, serverdir);

      rc = net_mount_nfs(config.mountpoint.extra, &config.net.server, serverdir);

      if(config.debug) fprintf(stderr, "nfs: err #2 = %d\n", rc);
    }
  }

  if(file && !rc) {
    config.extramount = 1;

    strprintf(&buf, "%s/%s", config.mountpoint.extra, file);
    rc = util_mount_ro(buf, config.mountpoint.instdata);

    fprintf(stderr, "nfs: err #3 = %d\n", rc);

    if(rc) {
      fprintf(stderr, "nfs: %s: not found\n", file);
      inst_umount();
      rc = 2;
    }
    else {
      file_type = 1;
      str_copy(&config.serverpath, serverdir);
      str_copy(&config.serverfile, file);
    }
  }

  str_copy(&serverdir, NULL);

  if(config.debug) fprintf(stderr, "nfs: err #4 = %d\n", rc);

  /* rc = -1 --> error was shown in net_mount_nfs() */
  if(rc == -2) {
    fprintf(stderr, "network setup failed\n");
    if(config.win) dia_message("Network setup failed", MSGTYPE_ERROR);
  }
  else if(rc > 0) {
    strprintf(&buf, "nfs: mount failed: %s", strerror(rc));

    fprintf(stderr, "%s\n", buf);
    if(config.win) dia_message(buf, MSGTYPE_ERROR);
  }

  if(config.win) {
    win_close(&win);
  }

  if(!rc) {
    fprintf(stderr, "nfs: mount ok\n");
    config.sourcetype = file_type;
  }

  str_copy(&buf, NULL);

  return rc;
}


/*
 * Mount smb install source.
 *
 * If config.serverdir points to a file, loop-mount the file.
 */
int do_mount_smb()
{
  int rc, file_type = 0;
  window_t win;
  char *buf = NULL;

  util_truncate_dir(config.serverdir);

  strprintf(&buf,
    config.win ? txt_get(TXT_SMB_TRYING_MOUNT) : "smb: trying to mount //%s/%s\n" ,
    config.net.server.name, config.net.share
  );

  if(config.win) {
    dia_info(&win, buf);
  }
  else {
    fprintf(stderr, "%s", buf);
  }

  mkdir(config.mountpoint.extra, 0755);

  rc = net_mount_smb(config.mountpoint.extra,
    &config.net.server, config.net.share,
    config.net.user, config.net.password, config.net.workgroup
  );

  if(!rc) {
    config.extramount = 1;
    strprintf(&buf, "%s/%s", config.mountpoint.extra, config.serverdir);
    if((file_type = util_check_exist(buf))) {
      rc = util_mount_ro(buf, config.mountpoint.instdata);
    }
    else {
      fprintf(stderr, "smb: %s: not found\n", config.serverdir);
      rc = -1;
    }
  }

  switch(rc) {
    case 0:
      break;

    case -3:
      fprintf(stderr, "smb: network setup failed\n");
      if(config.win) dia_message("Network setup failed", MSGTYPE_ERROR);
      break;

    case -2:
      fprintf(stderr, "smb: smb/cifs not supported\n");
      if(config.win) dia_message("SMB/CIFS not supported", MSGTYPE_ERROR);
      break;

    default:	/* -1 */
      fprintf(stderr, "smb: mount failed\n");
      if(config.win) dia_message("SMB/CIFS mount failed", MSGTYPE_ERROR);
  }

  if(config.win) win_close(&win);

  if(rc) {
    inst_umount();
  }
  else {
    fprintf(stderr, "smb: mount ok\n");
  }

  if(!rc) config.sourcetype = file_type == 'r' ? 1 : 0;

  str_copy(&buf, NULL);

  return rc;
}


/*
 * Mount disk install source.
 *
 * If config.serverdir points to a file, loop-mount file.
 *
 * disk_type: 0 = cd, 1 = hd (only used for error message)
 */
int do_mount_disk(char *dev, int disk_type)
{
  int rc = 0, file_type = 0;
  char *buf = NULL, *module, *type;
  char *dir;

  util_truncate_dir(config.serverdir);

  dir = config.serverdir;
  if(!dir || !*dir || !strcmp(dir, "/")) dir = NULL;

  /* load fs module if necessary */
  type = util_fstype(dev, &module);
  if(module) mod_modprobe(module, NULL);

  if(!type || !strcmp(type, "swap")) rc = -1;

  mkdir(config.mountpoint.extra, 0755);

  if(!rc) {
    rc = util_mount_ro(dev, dir ? config.mountpoint.extra : config.mountpoint.instdata);
  }

  if(rc) {
    fprintf(stderr, "disk: %s: mount failed\n", dev);
    if(config.win) dia_message(txt_get(disk_type ? TXT_ERROR_HD_MOUNT : TXT_ERROR_CD_MOUNT), MSGTYPE_ERROR);
  }
  else {
    if(dir) {
      config.extramount = 1;
      strprintf(&buf, "%s/%s", config.mountpoint.extra, dir);
      file_type = util_check_exist(buf);
      rc = file_type ? util_mount_ro(buf, config.mountpoint.instdata) : -1;

      if(rc) {
        fprintf(stderr, "disk: %s: not found\n", dir);
        if(config.win) dia_message(txt_get(TXT_RI_NOT_FOUND), MSGTYPE_ERROR);
        inst_umount();
      }
    }

    if(!rc) {
      fprintf(stderr, "disk: %s: mount ok\n", dev);
    }
  }

  if(!rc) config.sourcetype = file_type == 'r' ? 1 : 0;

  str_copy(&buf, NULL);

  return rc;
}


