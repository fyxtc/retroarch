/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2015-2016 - Andre Leiradella
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <ctype.h>

#include <file/file_path.h>
#include <string/stdstring.h>
#include <streams/interface_stream.h>
#include <streams/file_stream.h>
#include <features/features_cpu.h>
#include <compat/strl.h>
#include <rhash.h>
#include <retro_miscellaneous.h>
#include <retro_math.h>
#include <net/net_http.h>
#include <libretro.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#ifdef HAVE_MENU
#include "../menu/menu_driver.h"
#include "../menu/menu_entries.h"
#ifdef HAVE_MENU_WIDGETS
#include "../menu/widgets/menu_widgets.h"
#endif
#endif

#ifdef HAVE_THREADS
#include <rthreads/rthreads.h>
#endif

#include "badges.h"
#include "cheevos.h"
#include "fixup.h"
#include "parser.h"
#include "hash.h"
#include "util.h"

#include "../file_path_special.h"
#include "../paths.h"
#include "../command.h"
#include "../dynamic.h"
#include "../configuration.h"
#include "../performance_counters.h"
#include "../msg_hash.h"
#include "../retroarch.h"
#include "../core.h"

#include "../network/net_http_special.h"
#include "../tasks/tasks_internal.h"

#include "../deps/rcheevos/include/rcheevos.h"
#include "../deps/rcheevos/include/rurl.h"

/* Define this macro to prevent cheevos from being deactivated. */
#undef CHEEVOS_DONT_DEACTIVATE

/* Define this macro to log URLs (will log the user token). */
#undef CHEEVOS_LOG_URLS

/* Define this macro to dump all cheevos' addresses. */
#undef CHEEVOS_DUMP_ADDRS

/* Define this macro to remove HTTP timeouts. */
#undef CHEEVOS_NO_TIMEOUT

/* Define this macro to load a JSON file from disk instead of downloading
 * from retroachievements.org. */
#undef CHEEVOS_JSON_OVERRIDE

/* Define this macro with a string to save the JSON file to disk with
 * that name. */
#undef CHEEVOS_SAVE_JSON

/* Define this macro to have the password and token logged. THIS WILL DISCLOSE
 * THE USER'S PASSWORD, TAKE CARE! */
#undef CHEEVOS_LOG_PASSWORD

/* Define this macro to log downloaded badge images. */
#undef CHEEVOS_LOG_BADGES

typedef struct
{
   rc_trigger_t* trigger;
   const rcheevos_racheevo_t* info;
   int active;
   int last;
} rcheevos_cheevo_t;

typedef struct
{
   rc_lboard_t* lboard;
   const rcheevos_ralboard_t* info;
   bool active;
   unsigned last_value;
   int format;
} rcheevos_lboard_t;

typedef struct
{
   retro_task_t* task;
#ifdef HAVE_THREADS
   slock_t* task_lock;
#endif

   bool core_supports;

   rcheevos_rapatchdata_t patchdata;
   rcheevos_cheevo_t* core;
   rcheevos_cheevo_t* unofficial;
   rcheevos_lboard_t* lboards;

   rcheevos_fixups_t fixups;

   char token[32];
} rcheevos_locals_t;

typedef struct
{
   int label;
   const char* name;
   const uint32_t* ext_hashes;
} rcheevos_finder_t;

typedef struct
{
   uint8_t id[4]; /* NES^Z */
   uint8_t rom_size;
   uint8_t vrom_size;
   uint8_t rom_type;
   uint8_t rom_type2;
   uint8_t reserve[8];
} rcheevos_nes_header_t;

static rcheevos_locals_t rcheevos_locals =
{
   NULL, /* task */
#ifdef HAVE_THREADS
   NULL, /* task_lock */
#endif
   true, /* core_supports */
   {0},  /* patchdata */
   NULL, /* core */
   NULL, /* unofficial */
   NULL, /* lboards */
   {0},  /* fixups */
   {0},  /* token */
};

bool rcheevos_loaded = false;
bool rcheevos_hardcore_active = false;
bool rcheevos_hardcore_paused = false;
bool rcheevos_state_loaded_flag = false;
int rcheevos_cheats_are_enabled = 0;
int rcheevos_cheats_were_enabled = 0;

#ifdef HAVE_THREADS
#define CHEEVOS_LOCK(l)   do { slock_lock(l); } while (0)
#define CHEEVOS_UNLOCK(l) do { slock_unlock(l); } while (0)
#else
#define CHEEVOS_LOCK(l)
#define CHEEVOS_UNLOCK(l)
#endif

#define CHEEVOS_MB(x)   ((x) * 1024 * 1024)

/*****************************************************************************
Supporting functions.
*****************************************************************************/

#ifndef CHEEVOS_VERBOSE

void rcheevos_log(const char *fmt, ...)
{
   (void)fmt;
}

#endif

static void rcheevos_log_url(const char* format, const char* url)
{
#ifdef CHEEVOS_LOG_URLS
#ifdef CHEEVOS_LOG_PASSWORD
   CHEEVOS_LOG(format, url);
#else
   char copy[256];
   char* aux      = NULL;
   char* next     = NULL;

   if (!string_is_empty(url))
      strlcpy(copy, url, sizeof(copy));

   aux = strstr(copy, "?p=");

   if (!aux)
      aux = strstr(copy, "&p=");

   if (aux)
   {
      aux += 3;
      next = strchr(aux, '&');

      if (next)
      {
         do
         {
            *aux++ = *next++;
         } while (next[-1] != 0);
      }
      else
         *aux = 0;
   }

   aux = strstr(copy, "?t=");

   if (!aux)
      aux = strstr(copy, "&t=");

   if (aux)
   {
      aux += 3;
      next = strchr(aux, '&');

      if (next)
      {
         do
         {
            *aux++ = *next++;
         } while (next[-1] != 0);
      }
      else
         *aux = 0;
   }

   CHEEVOS_LOG(format, copy);
#endif
#else
   (void)format;
   (void)url;
#endif
}

static const char* rcheevos_rc_error(int ret)
{
   switch (ret)
   {
      case RC_OK: return "Ok";
      case RC_INVALID_LUA_OPERAND: return "Invalid Lua operand";
      case RC_INVALID_MEMORY_OPERAND: return "Invalid memory operand";
      case RC_INVALID_CONST_OPERAND: return "Invalid constant operand";
      case RC_INVALID_FP_OPERAND: return "Invalid floating-point operand";
      case RC_INVALID_CONDITION_TYPE: return "Invalid condition type";
      case RC_INVALID_OPERATOR: return "Invalid operator";
      case RC_INVALID_REQUIRED_HITS: return "Invalid required hits";
      case RC_DUPLICATED_START: return "Duplicated start condition";
      case RC_DUPLICATED_CANCEL: return "Duplicated cancel condition";
      case RC_DUPLICATED_SUBMIT: return "Duplicated submit condition";
      case RC_DUPLICATED_VALUE: return "Duplicated value expression";
      case RC_DUPLICATED_PROGRESS: return "Duplicated progress expression";
      case RC_MISSING_START: return "Missing start condition";
      case RC_MISSING_CANCEL: return "Missing cancel condition";
      case RC_MISSING_SUBMIT: return "Missing submit condition";
      case RC_MISSING_VALUE: return "Missing value expression";
      case RC_INVALID_LBOARD_FIELD: return "Invalid field in leaderboard";
      default: return "Unknown error";
   }
}

static int rcheevos_parse(const char* json)
{
   char buffer[256];
   settings_t *settings     = config_get_ptr();
   int res                  = 0;
   int i                    = 0;
   unsigned j               = 0;
   unsigned count           = 0;
   rcheevos_cheevo_t* cheevo = NULL;
   rcheevos_lboard_t* lboard = NULL;
   rcheevos_racheevo_t* rac  = NULL;

   rcheevos_fixup_init(&rcheevos_locals.fixups);

   res = rcheevos_get_patchdata(json, &rcheevos_locals.patchdata);

   if (res != 0)
   {
      RARCH_ERR(RCHEEVOS_TAG "Error parsing cheevos");
      return -1;
   }

   if (   rcheevos_locals.patchdata.core_count == 0
       && rcheevos_locals.patchdata.unofficial_count == 0
       && rcheevos_locals.patchdata.lboard_count == 0)
   {
      rcheevos_locals.core = NULL;
      rcheevos_locals.unofficial = NULL;
      rcheevos_locals.lboards = NULL;
      rcheevos_free_patchdata(&rcheevos_locals.patchdata);
      return 0;
   }

   /* Allocate memory. */
   rcheevos_locals.core = (rcheevos_cheevo_t*)
      calloc(rcheevos_locals.patchdata.core_count, sizeof(rcheevos_cheevo_t));

   rcheevos_locals.unofficial = (rcheevos_cheevo_t*)
      calloc(rcheevos_locals.patchdata.unofficial_count, sizeof(rcheevos_cheevo_t));

   rcheevos_locals.lboards = (rcheevos_lboard_t*)
      calloc(rcheevos_locals.patchdata.lboard_count, sizeof(rcheevos_lboard_t));

   if (   !rcheevos_locals.core
       || !rcheevos_locals.unofficial
       || !rcheevos_locals.lboards)
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Error allocating memory for cheevos");
      goto error;
   }

   /* Initialize. */
   for (i = 0; i < 2; i++)
   {
      if (i == 0)
      {
         cheevo = rcheevos_locals.core;
         rac    = rcheevos_locals.patchdata.core;
         count  = rcheevos_locals.patchdata.core_count;
      }
      else
      {
         cheevo = rcheevos_locals.unofficial;
         rac    = rcheevos_locals.patchdata.unofficial;
         count  = rcheevos_locals.patchdata.unofficial_count;
      }

      for (j = 0; j < count; j++, cheevo++, rac++)
      {
         cheevo->info = rac;
         res = rc_trigger_size(cheevo->info->memaddr);

         if (res < 0)
         {
            CHEEVOS_ERR(RCHEEVOS_TAG "Error in cheevo memaddr %s: %s",
               cheevo->info->memaddr, rcheevos_rc_error(res));
            goto error;
         }

         cheevo->trigger = (rc_trigger_t*)calloc(1, res);

         if (!cheevo->trigger)
         {
            CHEEVOS_ERR(RCHEEVOS_TAG "Error allocating memory for cheevos");
            goto error;
         }

         rc_parse_trigger(cheevo->trigger, cheevo->info->memaddr, NULL, 0);
         cheevo->active = RCHEEVOS_ACTIVE_SOFTCORE | RCHEEVOS_ACTIVE_HARDCORE;
         cheevo->last = 1;
      }
   }

   lboard = rcheevos_locals.lboards;
   count = rcheevos_locals.patchdata.lboard_count;

   for (j = 0; j < count; j++, lboard++)
   {
      lboard->info = rcheevos_locals.patchdata.lboards + j;
      res = rc_lboard_size(lboard->info->mem);

      if (res < 0)
      {
         snprintf(buffer, sizeof(buffer), "Error in leaderboard %d \"%s\": %s",
            lboard->info->id, lboard->info->title, rcheevos_rc_error(res));

         if (settings->bools.cheevos_verbose_enable)
            runloop_msg_queue_push(buffer, 0, 4 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);

         CHEEVOS_ERR(RCHEEVOS_TAG "%s mem: %s\n", buffer, lboard->info->mem);
         lboard->lboard = NULL;
         continue;
      }

      lboard->lboard = (rc_lboard_t*)calloc(1, res);

      if (!lboard->lboard)
      {
         CHEEVOS_ERR(RCHEEVOS_TAG "Error allocating memory for cheevos");
         goto error;
      }

      rc_parse_lboard(lboard->lboard,
         lboard->info->mem, NULL, 0);
      lboard->active = false;
      lboard->last_value = 0;
      lboard->format = rc_parse_format(lboard->info->format);
   }

   return 0;

error:
   CHEEVOS_FREE(rcheevos_locals.core);
   CHEEVOS_FREE(rcheevos_locals.unofficial);
   CHEEVOS_FREE(rcheevos_locals.lboards);
   rcheevos_free_patchdata(&rcheevos_locals.patchdata);
   rcheevos_fixup_destroy(&rcheevos_locals.fixups);
   return -1;
}

/*****************************************************************************
Test all the achievements (call once per frame).
*****************************************************************************/

static void rcheevos_award_task_softcore(retro_task_t *task, void* task_data, void* user_data,
      const char* error)
{
   settings_t *settings = config_get_ptr();
   const rcheevos_cheevo_t* cheevo = (const rcheevos_cheevo_t*)user_data;
   char buffer[256];
   int ret;
   buffer[0] = 0;

   if (error == NULL)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Awarded achievement %u\n", cheevo->info->id);
      return;
   }

   if (*error)
      CHEEVOS_ERR(RCHEEVOS_TAG "Error awarding achievement %u: %s\n", cheevo->info->id, error);

   /* Try again. */
   ret = rc_url_award_cheevo(buffer, sizeof(buffer), settings->arrays.cheevos_username, rcheevos_locals.token, cheevo->info->id, 0);

   if (ret != 0)
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Buffer to small to create URL");
      return;
   }

   rcheevos_log_url(RCHEEVOS_TAG "rc_url_award_cheevo: %s\n", buffer);
   task_push_http_transfer(buffer, true, NULL, rcheevos_award_task_softcore, user_data);
}

static void rcheevos_award_task_hardcore(retro_task_t *task, void* task_data, void* user_data,
      const char* error)
{
   settings_t *settings = config_get_ptr();
   const rcheevos_cheevo_t* cheevo = (const rcheevos_cheevo_t*)user_data;
   char buffer[256];
   int ret;
   buffer[0] = 0;

   if (error == NULL)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Awarded achievement %u\n", cheevo->info->id);
      return;
   }

   if (*error)
      CHEEVOS_ERR(RCHEEVOS_TAG "Error awarding achievement %u: %s\n", cheevo->info->id, error);

   /* Try again. */
   ret = rc_url_award_cheevo(buffer, sizeof(buffer), settings->arrays.cheevos_username, rcheevos_locals.token, cheevo->info->id, 1);

   if (ret != 0)
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Buffer to small to create URL\n");
      return;
   }

   rcheevos_log_url(RCHEEVOS_TAG "rc_url_award_cheevo: %s\n", buffer);
   task_push_http_transfer(buffer, true, NULL, rcheevos_award_task_hardcore, user_data);
}

static void rcheevos_award(rcheevos_cheevo_t* cheevo, int mode)
{
   settings_t *settings = config_get_ptr();
   char buffer[256];
   buffer[0] = 0;

   CHEEVOS_LOG(RCHEEVOS_TAG "awarding cheevo %u: %s (%s)\n",
         cheevo->info->id, cheevo->info->title, cheevo->info->description);

   /* Deactivates the cheevo. */
   cheevo->active &= ~mode;

   if (mode == RCHEEVOS_ACTIVE_HARDCORE)
      cheevo->active &= ~RCHEEVOS_ACTIVE_SOFTCORE;

   /* Show the OSD message. */
   {
#if defined(HAVE_MENU) && defined(HAVE_MENU_WIDGETS)
      bool widgets_ready = menu_widgets_ready();
      if (widgets_ready)
         menu_widgets_push_achievement(cheevo->info->title, cheevo->info->badge);
      else
#endif
      {
         snprintf(buffer, sizeof(buffer), "Achievement Unlocked: %s", cheevo->info->title);
         runloop_msg_queue_push(buffer, 0, 2 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         runloop_msg_queue_push(cheevo->info->description, 0, 3 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
      }
   }

   /* Start the award task. */
   if ((mode & RCHEEVOS_ACTIVE_HARDCORE) != 0)
      rcheevos_award_task_hardcore(NULL, NULL, cheevo, "");
   else
      rcheevos_award_task_softcore(NULL, NULL, cheevo, "");

   /* Take a screenshot of the achievement. */
   if (settings && settings->bools.cheevos_auto_screenshot)
   {
      char shotname[8192];

      snprintf(shotname, sizeof(shotname), "%s/%s-cheevo-%u",
      settings->paths.directory_screenshot,
      path_basename(path_get(RARCH_PATH_BASENAME)),
      cheevo->info->id);
      shotname[sizeof(shotname) - 1] = '\0';

      if (take_screenshot(settings->paths.directory_screenshot,
               shotname, true,
               video_driver_cached_frame_has_valid_framebuffer(), false, true))
         CHEEVOS_LOG(RCHEEVOS_TAG "got a screenshot for cheevo %u\n", cheevo->info->id);
      else
         CHEEVOS_LOG(RCHEEVOS_TAG "failed to get screenshot for cheevo %u\n", cheevo->info->id);
   }
}

static unsigned rcheevos_peek(unsigned address, unsigned num_bytes, void* ud)
{
   const uint8_t* data = rcheevos_fixup_find(&rcheevos_locals.fixups,
      address, rcheevos_locals.patchdata.console_id);
   unsigned value = 0;

   if (data)
   {
      switch (num_bytes)
      {
         case 4: value |= data[2] << 16 | data[3] << 24;
         case 2: value |= data[1] << 8;
         case 1: value |= data[0];
      }
   }

   return value;
}

static void rcheevos_test_cheevo_set(bool official)
{
   settings_t *settings = config_get_ptr();
   int mode = RCHEEVOS_ACTIVE_SOFTCORE;
   rcheevos_cheevo_t* cheevo;
   int i, count;

   if (settings && settings->bools.cheevos_hardcore_mode_enable && !rcheevos_hardcore_paused)
      mode = RCHEEVOS_ACTIVE_HARDCORE;

   if (official)
   {
      cheevo = rcheevos_locals.core;
      count = rcheevos_locals.patchdata.core_count;
   }
   else
   {
      cheevo = rcheevos_locals.unofficial;
      count = rcheevos_locals.patchdata.unofficial_count;
   }

   for (i = 0; i < count; i++, cheevo++)
   {
      /* Check if the achievement is active for the current mode. */
      if ((cheevo->active & mode) == 0)
         continue;

      if (cheevo->active & mode)
      {
         int valid = rc_test_trigger(cheevo->trigger, rcheevos_peek, NULL, NULL);

         if (cheevo->last)
            rc_reset_trigger(cheevo->trigger);
         else if (valid)
            rcheevos_award(cheevo, mode);

         cheevo->last = valid;
      }
   }
}

static void rcheevos_lboard_submit_task(retro_task_t *task, void* task_data, void* user_data,
      const char* error)
{
   settings_t *settings = config_get_ptr();
   const rcheevos_lboard_t* lboard = (const rcheevos_lboard_t*)user_data;
   MD5_CTX ctx;
   uint8_t hash[16];
   char signature[64];
   char buffer[256];
   int ret;

   if (!error)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Submitted leaderboard %u\n", lboard->info->id);
      return;
   }

   CHEEVOS_ERR(RCHEEVOS_TAG "Error submitting leaderboard %u: %s\n", lboard->info->id, error);

   /* Try again. */

   /* Evaluate the signature. */
   snprintf(signature, sizeof(signature), "%u%s%u", lboard->info->id,
      settings->arrays.cheevos_username,
      lboard->info->id);

   MD5_Init(&ctx);
   MD5_Update(&ctx, (void*)signature, strlen(signature));
   MD5_Final(hash, &ctx);

   /* Start the request. */
   ret = rc_url_submit_lboard(buffer, sizeof(buffer), settings->arrays.cheevos_username, rcheevos_locals.token, lboard->info->id, lboard->last_value, hash);

   if (ret != 0)
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Buffer to small to create URL\n");
      return;
   }

   rcheevos_log_url(RCHEEVOS_TAG "rc_url_submit_lboard: %s\n", buffer);
   task_push_http_transfer(buffer, true, NULL, rcheevos_lboard_submit_task, user_data);
}

static void rcheevos_lboard_submit(rcheevos_lboard_t* lboard)
{
   char buffer[256];
   char value[16];

   /* Deactivate the leaderboard. */
   lboard->active = 0;

   /* Failsafe for improper leaderboards. */
   if (lboard->last_value == 0)
   {
      CHEEVOS_ERR(RCHEEVOS_TAG "Leaderboard %s tried to submit 0\n", lboard->info->title);
      runloop_msg_queue_push("Leaderboard attempt cancelled!", 0, 2 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
      return;
   }

   /* Show the OSD message. */
   rc_format_value(value, sizeof(value), lboard->last_value, lboard->format);

   snprintf(buffer, sizeof(buffer), "Submitted %s for %s",
         value, lboard->info->title);
   runloop_msg_queue_push(buffer, 0, 2 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);

   /* Start the submit task. */
   rcheevos_lboard_submit_task(NULL, NULL, lboard, "no error, first try");
}

static void rcheevos_test_leaderboards(void)
{
   rcheevos_lboard_t* lboard = rcheevos_locals.lboards;
   unsigned	 i;

   for (i = 0; i < rcheevos_locals.patchdata.lboard_count; i++, lboard++)
   {
      if (!lboard->lboard) continue;
      switch (rc_evaluate_lboard(lboard->lboard, &lboard->last_value, rcheevos_peek, NULL, NULL))
      {
         default:
         case RC_LBOARD_INACTIVE:
            break;

         case RC_LBOARD_ACTIVE:
            /* this is where we would update the onscreen tracker */
            break;

         case RC_LBOARD_TRIGGERED:
            rcheevos_lboard_submit(lboard);
            break;

         case RC_LBOARD_CANCELED:
         {
            CHEEVOS_LOG(RCHEEVOS_TAG "Cancel leaderboard %s\n", lboard->info->title);
            lboard->active = 0;
            runloop_msg_queue_push("Leaderboard attempt cancelled!",
                  0, 2 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
            break;
         }

         case RC_LBOARD_STARTED:
         {
            char buffer[256];

            CHEEVOS_LOG(RCHEEVOS_TAG "Leaderboard started: %s\n", lboard->info->title);
            lboard->active     = 1;
            lboard->last_value = 0;

            snprintf(buffer, sizeof(buffer),
                  "Leaderboard Active: %s", lboard->info->title);
            runloop_msg_queue_push(buffer, 0, 2 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
            runloop_msg_queue_push(lboard->info->description, 0, 3 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
            break;
         }
      }
   }
}

void rcheevos_reset_game(void)
{
   rcheevos_cheevo_t* cheevo;
   rcheevos_lboard_t* lboard;
   unsigned i;

   cheevo = rcheevos_locals.core;
   for (i = 0; i < rcheevos_locals.patchdata.core_count; i++, cheevo++)
   {
      cheevo->last = 1;
   }

   cheevo = rcheevos_locals.unofficial;
   for (i = 0; i < rcheevos_locals.patchdata.unofficial_count; i++, cheevo++)
   {
      cheevo->last = 1;
   }

   lboard = rcheevos_locals.lboards;
   for (i = 0; i < rcheevos_locals.patchdata.lboard_count; i++, lboard++)
   {
      lboard->active = 0;
   }
}

void rcheevos_populate_menu(void* data)
{
#ifdef HAVE_MENU
   int i                         = 0;
   int count                     = 0;
   settings_t* settings          = config_get_ptr();
   menu_displaylist_info_t* info = (menu_displaylist_info_t*)data;
   rcheevos_cheevo_t* cheevo      = NULL;

   if (   settings->bools.cheevos_enable
       && settings->bools.cheevos_hardcore_mode_enable
       && rcheevos_loaded)
   {
      if (!rcheevos_hardcore_paused)
         menu_entries_append_enum(info->list,
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_ACHIEVEMENT_PAUSE),
               msg_hash_to_str(MENU_ENUM_LABEL_ACHIEVEMENT_PAUSE),
               MENU_ENUM_LABEL_ACHIEVEMENT_PAUSE,
               MENU_SETTING_ACTION_PAUSE_ACHIEVEMENTS, 0, 0);
      else
         menu_entries_append_enum(info->list,
               msg_hash_to_str(MENU_ENUM_LABEL_VALUE_ACHIEVEMENT_RESUME),
               msg_hash_to_str(MENU_ENUM_LABEL_ACHIEVEMENT_RESUME),
               MENU_ENUM_LABEL_ACHIEVEMENT_RESUME,
               MENU_SETTING_ACTION_RESUME_ACHIEVEMENTS, 0, 0);
   }

   cheevo = rcheevos_locals.core;

   for (i = 0, count = rcheevos_locals.patchdata.core_count; i < count; i++, cheevo++)
   {
      if (!(cheevo->active & RCHEEVOS_ACTIVE_HARDCORE))
      {
         menu_entries_append_enum(info->list, cheevo->info->title,
            cheevo->info->description,
            MENU_ENUM_LABEL_CHEEVOS_UNLOCKED_ENTRY_HARDCORE,
            MENU_SETTINGS_CHEEVOS_START + i, 0, 0);

         set_badge_info(&badges_ctx, i, cheevo->info->badge,
            (cheevo->active & RCHEEVOS_ACTIVE_HARDCORE));
      }
      else if (!(cheevo->active & RCHEEVOS_ACTIVE_SOFTCORE))
      {
         menu_entries_append_enum(info->list, cheevo->info->title,
            cheevo->info->description,
            MENU_ENUM_LABEL_CHEEVOS_UNLOCKED_ENTRY,
            MENU_SETTINGS_CHEEVOS_START + i, 0, 0);

         set_badge_info(&badges_ctx, i, cheevo->info->badge,
            (cheevo->active & RCHEEVOS_ACTIVE_SOFTCORE));
      }
      else
      {
         menu_entries_append_enum(info->list, cheevo->info->title,
            cheevo->info->description,
            MENU_ENUM_LABEL_CHEEVOS_LOCKED_ENTRY,
            MENU_SETTINGS_CHEEVOS_START + i, 0, 0);

         set_badge_info(&badges_ctx, i, cheevo->info->badge,
            (cheevo->active & RCHEEVOS_ACTIVE_SOFTCORE));
      }
   }

   if (settings->bools.cheevos_test_unofficial)
   {
      cheevo = rcheevos_locals.unofficial;

      for (i = 0, count = rcheevos_locals.patchdata.unofficial_count; i < count; i++, cheevo++)
      {
         if (!(cheevo->active & RCHEEVOS_ACTIVE_HARDCORE))
         {
            menu_entries_append_enum(info->list, cheevo->info->title,
               cheevo->info->description,
               MENU_ENUM_LABEL_CHEEVOS_UNLOCKED_ENTRY_HARDCORE,
               MENU_SETTINGS_CHEEVOS_START + i, 0, 0);

            set_badge_info(&badges_ctx, i, cheevo->info->badge,
               (cheevo->active & RCHEEVOS_ACTIVE_HARDCORE));
         }
         else if (!(cheevo->active & RCHEEVOS_ACTIVE_SOFTCORE))
         {
            menu_entries_append_enum(info->list, cheevo->info->title,
               cheevo->info->description,
               MENU_ENUM_LABEL_CHEEVOS_UNLOCKED_ENTRY,
               MENU_SETTINGS_CHEEVOS_START + i, 0, 0);

            set_badge_info(&badges_ctx, i, cheevo->info->badge,
               (cheevo->active & RCHEEVOS_ACTIVE_SOFTCORE));
         }
         else
         {
            menu_entries_append_enum(info->list, cheevo->info->title,
               cheevo->info->description,
               MENU_ENUM_LABEL_CHEEVOS_LOCKED_ENTRY,
               MENU_SETTINGS_CHEEVOS_START + i, 0, 0);

            set_badge_info(&badges_ctx, i, cheevo->info->badge,
               (cheevo->active & RCHEEVOS_ACTIVE_SOFTCORE));
         }
      }
   }

   count = rcheevos_locals.patchdata.core_count;

   if (settings->bools.cheevos_test_unofficial)
      count += rcheevos_locals.patchdata.unofficial_count;

   if (count == 0)
   {
      menu_entries_append_enum(info->list,
            msg_hash_to_str(MENU_ENUM_LABEL_VALUE_NO_ACHIEVEMENTS_TO_DISPLAY),
            msg_hash_to_str(MENU_ENUM_LABEL_NO_ACHIEVEMENTS_TO_DISPLAY),
            MENU_ENUM_LABEL_NO_ACHIEVEMENTS_TO_DISPLAY,
            FILE_TYPE_NONE, 0, 0);
   }
#endif
}

bool rcheevos_get_description(rcheevos_ctx_desc_t* desc)
{
   unsigned idx;
   const rcheevos_cheevo_t* cheevo;

   if (!desc)
      return false;

   *desc->s = 0;

   if (rcheevos_loaded)
   {
      idx = desc->idx;

      if (idx < rcheevos_locals.patchdata.core_count)
         cheevo = rcheevos_locals.core + idx;
      else
      {
         idx -= rcheevos_locals.patchdata.core_count;

         if (idx < rcheevos_locals.patchdata.unofficial_count)
            cheevo = rcheevos_locals.unofficial + idx;
         else
            return true;
      }

      strlcpy(desc->s, cheevo->info->description, desc->len);
   }

   return true;
}

bool rcheevos_apply_cheats(bool* data_bool)
{
   rcheevos_cheats_are_enabled   = *data_bool;
   rcheevos_cheats_were_enabled |= rcheevos_cheats_are_enabled;

   return true;
}

bool rcheevos_unload(void)
{
   bool running = false;
   unsigned i = 0, count = 0;

   CHEEVOS_LOCK(rcheevos_locals.task_lock);
   running = rcheevos_locals.task != NULL;
   CHEEVOS_UNLOCK(rcheevos_locals.task_lock);

   if (running)
   {
      CHEEVOS_LOG(RCHEEVOS_TAG "Asked the load thread to terminate\n");
      task_queue_cancel_task(rcheevos_locals.task);

#ifdef HAVE_THREADS
      do
      {
         CHEEVOS_LOCK(rcheevos_locals.task_lock);
         running = rcheevos_locals.task != NULL;
         CHEEVOS_UNLOCK(rcheevos_locals.task_lock);
      }
      while (running);
#endif
   }

   if (rcheevos_loaded)
   {
      for (i = 0, count = rcheevos_locals.patchdata.core_count; i < count; i++)
      {
         CHEEVOS_FREE(rcheevos_locals.core[i].trigger);
      }

      for (i = 0, count = rcheevos_locals.patchdata.unofficial_count; i < count; i++)
      {
         CHEEVOS_FREE(rcheevos_locals.unofficial[i].trigger);
      }

      for (i = 0, count = rcheevos_locals.patchdata.lboard_count; i < count; i++)
      {
         CHEEVOS_FREE(rcheevos_locals.lboards[i].lboard);
      }

      CHEEVOS_FREE(rcheevos_locals.core);
      CHEEVOS_FREE(rcheevos_locals.unofficial);
      CHEEVOS_FREE(rcheevos_locals.lboards);
      rcheevos_free_patchdata(&rcheevos_locals.patchdata);
      rcheevos_fixup_destroy(&rcheevos_locals.fixups);

      rcheevos_locals.core       = NULL;
      rcheevos_locals.unofficial = NULL;
      rcheevos_locals.lboards    = NULL;

      rcheevos_loaded            = false;
      rcheevos_hardcore_active   = false;
      rcheevos_hardcore_paused   = false;
      rcheevos_state_loaded_flag = false;
   }

   return true;
}

bool rcheevos_toggle_hardcore_mode(void)
{
   settings_t *settings = config_get_ptr();

   if (!settings)
      return false;

   /* reset and deinit rewind to avoid cheat the score */
   if (   settings->bools.cheevos_hardcore_mode_enable
       && !rcheevos_hardcore_paused)
   {
      const char *msg = msg_hash_to_str(
            MSG_CHEEVOS_HARDCORE_MODE_ENABLE);

      /* reset the state loaded flag in case it was set */
      rcheevos_state_loaded_flag = false;

      /* send reset core cmd to avoid any user
       * savestate previusly loaded. */
      command_event(CMD_EVENT_RESET, NULL);

      if (settings->bools.rewind_enable)
         command_event(CMD_EVENT_REWIND_DEINIT, NULL);

      CHEEVOS_LOG("%s\n", msg);
      runloop_msg_queue_push(msg, 0, 3 * 60, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
   }
   else
   {
      if (settings->bools.rewind_enable)
         command_event(CMD_EVENT_REWIND_INIT, NULL);
   }

   return true;
}

void rcheevos_test(void)
{
   settings_t *settings = config_get_ptr();

   rcheevos_test_cheevo_set(true);

   if (settings)
   {
      if (settings->bools.cheevos_test_unofficial)
         rcheevos_test_cheevo_set(false);

      if (settings->bools.cheevos_hardcore_mode_enable &&
          settings->bools.cheevos_leaderboards_enable  &&
          !rcheevos_hardcore_paused)
         rcheevos_test_leaderboards();
   }
}

bool rcheevos_set_cheats(void)
{
   rcheevos_cheats_were_enabled = rcheevos_cheats_are_enabled;
   return true;
}

void rcheevos_set_support_cheevos(bool state)
{
   rcheevos_locals.core_supports = state;
}

bool rcheevos_get_support_cheevos(void)
{
  return rcheevos_locals.core_supports;
}

int rcheevos_get_console(void)
{
   return rcheevos_locals.patchdata.console_id;
}

static void rcheevos_unlock_cb(unsigned id, void* userdata)
{
   rcheevos_cheevo_t* cheevo = NULL;
   int i = 0;
   unsigned j = 0, count = 0;

   for (i = 0; i < 2; i++)
   {
      if (i == 0)
      {
         cheevo = rcheevos_locals.core;
         count = rcheevos_locals.patchdata.core_count;
      }
      else
      {
         cheevo = rcheevos_locals.unofficial;
         count = rcheevos_locals.patchdata.unofficial_count;
      }

      for (j = 0; j < count; j++, cheevo++)
      {
         if (cheevo->info->id == id)
         {
#ifndef CHEEVOS_DONT_DEACTIVATE
            cheevo->active &= ~*(unsigned*)userdata;
#endif
            CHEEVOS_LOG(RCHEEVOS_TAG "cheevo %u deactivated: %s\n", id, cheevo->info->title);
            return;
         }
      }
   }
}

#include "coro.h"

/* Uncomment the following two lines to debug rcheevos_iterate, this will
 * disable the coroutine yielding.
 *
 * The code is very easy to understand. It's meant to be like BASIC:
 * CORO_GOTO will jump execution to another label, CORO_GOSUB will
 * call another label, and CORO_RET will return from a CORO_GOSUB.
 *
 * This coroutine code is inspired in a very old pure C implementation
 * that runs everywhere:
 *
 * https://www.chiark.greenend.org.uk/~sgtatham/coroutines.html
 */
/*#undef CORO_YIELD
#define CORO_YIELD()*/

typedef struct
{
   /* variables used in the co-routine */
   char badge_name[16];
   char url[256];
   char badge_basepath[PATH_MAX_LENGTH];
   char badge_fullpath[PATH_MAX_LENGTH];
   unsigned char hash[16];
   bool round;
   unsigned gameid;
   unsigned i;
   unsigned j;
   unsigned k;
   size_t bytes;
   size_t count;
   size_t offset;
   size_t len;
   size_t size;
   MD5_CTX md5;
   rcheevos_nes_header_t header;
   retro_time_t t0;
   struct retro_system_info sysinfo;
   void *data;
   char *json;
   const char *path;
   const char *ext;
   intfstream_t *stream;
   rcheevos_cheevo_t *cheevo;
   settings_t *settings;
   struct http_connection_t *conn;
   struct http_t *http;
   const rcheevos_cheevo_t *cheevo_end;

   /* co-routine required fields */
   CORO_FIELDS
} rcheevos_coro_t;

enum
{
   /* Negative values because CORO_SUB generates positive values */
   RCHEEVOS_SNES_MD5     = -1,
   RCHEEVOS_GENESIS_MD5  = -2,
   RCHEEVOS_LYNX_MD5     = -3,
   RCHEEVOS_NES_MD5      = -4,
   RCHEEVOS_GENERIC_MD5  = -5,
   RCHEEVOS_FILENAME_MD5 = -6,
   RCHEEVOS_EVAL_MD5     = -7,
   RCHEEVOS_FILL_MD5     = -8,
   RCHEEVOS_GET_GAMEID   = -9,
   RCHEEVOS_GET_CHEEVOS  = -10,
   RCHEEVOS_GET_BADGES   = -11,
   RCHEEVOS_LOGIN        = -12,
   RCHEEVOS_HTTP_GET     = -13,
   RCHEEVOS_DEACTIVATE   = -14,
   RCHEEVOS_PLAYING      = -15,
   RCHEEVOS_DELAY        = -16
};

static int rcheevos_iterate(rcheevos_coro_t* coro)
{
   const int snes_header_len = 0x200;
   const int lynx_header_len = 0x40;
   ssize_t num_read = 0;
   size_t to_read   = 4096;
   uint8_t *buffer  = NULL;
   const char *end  = NULL;

   static const uint32_t genesis_exts[] =
   {
      0x0b888feeU, /* mdx */
      0x005978b6U, /* md  */
      0x0b88aa89U, /* smd */
      0x0b88767fU, /* gen */
      0x0b8861beU, /* bin */
      0x0b886782U, /* cue */
      0x0b8880d0U, /* iso */
      0x0b88aa98U, /* sms */
      0x005977f3U, /* gg  */
      0x0059797fU, /* sg  */
      0
   };

   static const uint32_t snes_exts[] =
   {
      0x0b88aa88U, /* smc */
      0x0b8872bbU, /* fig */
      0x0b88a9a1U, /* sfc */
      0x0b887623U, /* gd3 */
      0x0b887627U, /* gd7 */
      0x0b886bf3U, /* dx2 */
      0x0b886312U, /* bsx */
      0x0b88abd2U, /* swc */
      0
   };

   static const uint32_t lynx_exts[] =
   {
      0x0b888cf7U, /* lnx */
      0
   };

   static rcheevos_finder_t finders[] =
   {
      {RCHEEVOS_SNES_MD5,    "SNES (discards header)",            snes_exts},
      {RCHEEVOS_GENESIS_MD5, "Genesis (6Mb padding)",             genesis_exts},
      {RCHEEVOS_LYNX_MD5,    "Atari Lynx (discards header)", lynx_exts},
      {RCHEEVOS_NES_MD5,     "NES (discards header)",             NULL},
      {RCHEEVOS_GENERIC_MD5, "Generic (plain content)",           NULL},
      {RCHEEVOS_FILENAME_MD5, "Generic (filename)",               NULL}
   };

   CORO_ENTER();

      coro->settings = config_get_ptr();

      /* Bail out if cheevos are disabled.
         * But set the above anyways,
         * command_read_ram needs it. */
      if (!coro->settings->bools.cheevos_enable)
         CORO_STOP();

      /* Load the content into memory, or copy it
         * over to our own buffer */
      if (!coro->data)
      {
         coro->stream = intfstream_open_file(
               coro->path,
               RETRO_VFS_FILE_ACCESS_READ,
               RETRO_VFS_FILE_ACCESS_HINT_NONE);

         if (!coro->stream)
            CORO_STOP();

         CORO_YIELD();
         coro->len         = 0;
         coro->count       = intfstream_get_size(coro->stream);

         /* size limit */
         if (coro->count > CHEEVOS_MB(64))
            coro->count    = CHEEVOS_MB(64);

         coro->data        = malloc(coro->count);

         if (!coro->data)
         {
            intfstream_close(coro->stream);
            CHEEVOS_FREE(coro->stream);
            CORO_STOP();
         }

         for (;;)
         {
            buffer   = (uint8_t*)coro->data + coro->len;
            to_read  = 4096;

            if (to_read > coro->count)
               to_read = coro->count;

            num_read = intfstream_read(coro->stream,
                  (void*)buffer, to_read);

            if (num_read <= 0)
               break;

            coro->len         += num_read;
            coro->count       -= num_read;

            if (coro->count == 0)
               break;

            CORO_YIELD();
         }

         intfstream_close(coro->stream);
         CHEEVOS_FREE(coro->stream);
      }

      /* Use the supported extensions as a hint
         * to what method we should use. */
      core_get_system_info(&coro->sysinfo);

      for (coro->i = 0; coro->i < ARRAY_SIZE(finders); coro->i++)
      {
         if (finders[coro->i].ext_hashes)
         {
            coro->ext = coro->sysinfo.valid_extensions;

            while (coro->ext)
            {
               unsigned hash;
               end = strchr(coro->ext, '|');

               if (end)
               {
                  hash = rcheevos_djb2(coro->ext, end - coro->ext);
                  coro->ext = end + 1;
               }
               else
               {
                  hash = rcheevos_djb2(coro->ext, strlen(coro->ext));
                  coro->ext = NULL;
               }

               for (coro->j = 0; finders[coro->i].ext_hashes[coro->j]; coro->j++)
               {
                  if (finders[coro->i].ext_hashes[coro->j] == hash)
                  {
                     CHEEVOS_LOG(RCHEEVOS_TAG "testing %s\n",
                           finders[coro->i].name);

                     /*
                        * Inputs:  CHEEVOS_VAR_INFO
                        * Outputs: CHEEVOS_VAR_GAMEID, the game was found if it's different from 0
                        */
                     CORO_GOSUB(finders[coro->i].label);

                     if (coro->gameid != 0)
                        goto found;

                     coro->ext = NULL; /* force next finder */
                     break;
                  }
               }
            }
         }
      }

      for (coro->i = 0; coro->i < ARRAY_SIZE(finders); coro->i++)
      {
         if (finders[coro->i].ext_hashes)
            continue;

         CHEEVOS_LOG(RCHEEVOS_TAG "testing %s\n",
               finders[coro->i].name);

         /*
            * Inputs:  CHEEVOS_VAR_INFO
            * Outputs: CHEEVOS_VAR_GAMEID
            */
         CORO_GOSUB(finders[coro->i].label);

         if (coro->gameid != 0)
            goto found;
      }

      CHEEVOS_LOG(RCHEEVOS_TAG "this game doesn't feature achievements\n");
      CORO_STOP();

found:

#ifdef CHEEVOS_JSON_OVERRIDE
      {
         size_t size = 0;
         FILE *file  = fopen(CHEEVOS_JSON_OVERRIDE, "rb");

         fseek(file, 0, SEEK_END);
         size = ftell(file);
         fseek(file, 0, SEEK_SET);

         coro->json = (char*)malloc(size + 1);
         fread((void*)coro->json, 1, size, file);

         fclose(file);
         coro->json[size] = 0;
      }
#else
      CORO_GOSUB(RCHEEVOS_GET_CHEEVOS);

      if (!coro->json)
      {
         runloop_msg_queue_push("Error loading achievements.", 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         CHEEVOS_ERR(RCHEEVOS_TAG "error loading achievements\n");
         CORO_STOP();
      }
#endif

#ifdef CHEEVOS_SAVE_JSON
      {
         FILE *file = fopen(CHEEVOS_SAVE_JSON, "w");
         fwrite((void*)coro->json, 1, strlen(coro->json), file);
         fclose(file);
      }
#endif
      if (rcheevos_parse(coro->json))
      {
         CHEEVOS_FREE(coro->json);
         CORO_STOP();
      }

      CHEEVOS_FREE(coro->json);

      if (   rcheevos_locals.patchdata.core_count == 0
          && rcheevos_locals.patchdata.unofficial_count == 0
          && rcheevos_locals.patchdata.lboard_count == 0)
      {
         runloop_msg_queue_push(
               "This game has no achievements.",
               0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);

         CORO_STOP();
      }

      rcheevos_loaded = true;

      /*
         * Inputs:  CHEEVOS_VAR_GAMEID
         * Outputs:
         */
      CORO_GOSUB(RCHEEVOS_DEACTIVATE);

      /*
         * Inputs:  CHEEVOS_VAR_GAMEID
         * Outputs:
         */
      CORO_GOSUB(RCHEEVOS_PLAYING);

      if (coro->settings->bools.cheevos_verbose_enable && rcheevos_locals.patchdata.core_count > 0)
      {
         char msg[256];
         int mode               = RCHEEVOS_ACTIVE_SOFTCORE;
         const rcheevos_cheevo_t* cheevo = rcheevos_locals.core;
         const rcheevos_cheevo_t* end    = cheevo + rcheevos_locals.patchdata.core_count;
         int number_of_unlocked = rcheevos_locals.patchdata.core_count;

         if (coro->settings->bools.cheevos_hardcore_mode_enable && !rcheevos_hardcore_paused)
            mode = RCHEEVOS_ACTIVE_HARDCORE;

         for (; cheevo < end; cheevo++)
            if (cheevo->active & mode)
               number_of_unlocked--;

         snprintf(msg, sizeof(msg),
               "You have %d of %d achievements unlocked.",
               number_of_unlocked, rcheevos_locals.patchdata.core_count);
         msg[sizeof(msg) - 1] = 0;
         runloop_msg_queue_push(msg, 0, 6 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
      }

      CORO_GOSUB(RCHEEVOS_GET_BADGES);
      CORO_STOP();

      /**************************************************************************
       * Info   Tries to identify a SNES game
         * Input  CHEEVOS_VAR_INFO the content info
         * Output CHEEVOS_VAR_GAMEID the Retro Achievements game ID, or 0 if not found
         *************************************************************************/
   CORO_SUB(RCHEEVOS_SNES_MD5)
      MD5_Init(&coro->md5);

      /* Checks for the existence of a headered SNES file.
         Unheadered files fall back to RCHEEVOS_GENERIC_MD5. */
      if (coro->len < 0x2000 || coro->len % 0x2000 != snes_header_len)
      {
         coro->gameid = 0;
         CORO_RET();
      }

      coro->offset = snes_header_len;
      coro->count  = 0;

      CORO_GOSUB(RCHEEVOS_EVAL_MD5);
      MD5_Final(coro->hash, &coro->md5);

      CORO_GOTO(RCHEEVOS_GET_GAMEID);

      /**************************************************************************
       * Info   Tries to identify a Genesis game
         * Input  CHEEVOS_VAR_INFO the content info
         * Output CHEEVOS_VAR_GAMEID the Retro Achievements game ID, or 0 if not found
         *************************************************************************/
   CORO_SUB(RCHEEVOS_GENESIS_MD5)

      MD5_Init(&coro->md5);

      coro->offset = 0;
      coro->count  = 0;
      CORO_GOSUB(RCHEEVOS_EVAL_MD5);

      if (coro->count == 0)
      {
         MD5_Final(coro->hash, &coro->md5);
         coro->gameid = 0;
         CORO_RET();
      }

      if (coro->count < CHEEVOS_MB(6))
      {
         coro->offset = 0;
         coro->count  = CHEEVOS_MB(6) - coro->count;
         CORO_GOSUB(RCHEEVOS_FILL_MD5);
      }

      MD5_Final(coro->hash, &coro->md5);
      CORO_GOTO(RCHEEVOS_GET_GAMEID);

      /**************************************************************************
       * Info   Tries to identify an Atari Lynx game
         * Input  CHEEVOS_VAR_INFO the content info
         * Output CHEEVOS_VAR_GAMEID the Retro Achievements game ID, or 0 if not found
         *************************************************************************/
   CORO_SUB(RCHEEVOS_LYNX_MD5)

      /* Checks for the existence of a headered Lynx file.
         Unheadered files fall back to RCHEEVOS_GENERIC_MD5. */
      if (coro->len <= (unsigned)lynx_header_len ||
        memcmp("LYNX", (void *)coro->data, 5) != 0)
      {
         coro->gameid = 0;
         CORO_RET();
      }

      MD5_Init(&coro->md5);
      coro->offset = lynx_header_len;
      coro->count  = coro->len - lynx_header_len;
      CORO_GOSUB(RCHEEVOS_EVAL_MD5);

      MD5_Final(coro->hash, &coro->md5);
      CORO_GOTO(RCHEEVOS_GET_GAMEID);

      /**************************************************************************
       * Info   Tries to identify a NES game
         * Input  CHEEVOS_VAR_INFO the content info
         * Output CHEEVOS_VAR_GAMEID the Retro Achievements game ID, or 0 if not found
         *************************************************************************/
   CORO_SUB(RCHEEVOS_NES_MD5)

      /* Checks for the existence of a headered NES file.
         Unheadered files fall back to RCHEEVOS_GENERIC_MD5. */
      if (coro->len < sizeof(coro->header))
      {
         coro->gameid = 0;
         CORO_RET();
      }

      memcpy((void*)&coro->header, coro->data,
            sizeof(coro->header));

      if (     coro->header.id[0] != 'N'
            || coro->header.id[1] != 'E'
            || coro->header.id[2] != 'S'
            || coro->header.id[3] != 0x1a)
      {
         coro->gameid = 0;
         CORO_RET();
      }

      MD5_Init(&coro->md5);
      coro->offset = sizeof(coro->header);
      coro->count  = coro->len - coro->offset;
      CORO_GOSUB(RCHEEVOS_EVAL_MD5);

      MD5_Final(coro->hash, &coro->md5);
      CORO_GOTO(RCHEEVOS_GET_GAMEID);

      /**************************************************************************
       * Info   Tries to identify a "generic" game
         * Input  CHEEVOS_VAR_INFO the content info
         * Output CHEEVOS_VAR_GAMEID the Retro Achievements game ID, or 0 if not found
         *************************************************************************/
   CORO_SUB(RCHEEVOS_GENERIC_MD5)

      MD5_Init(&coro->md5);

      coro->offset      = 0;
      coro->count       = 0;
      CORO_GOSUB(RCHEEVOS_EVAL_MD5);

      MD5_Final(coro->hash, &coro->md5);

      if (coro->count == 0)
         CORO_RET();

      CORO_GOTO(RCHEEVOS_GET_GAMEID);

      /**************************************************************************
       * Info  Tries to identify a game based on its filename (with no extension)
         * Input  CHEEVOS_VAR_INFO the content info
         * Output CHEEVOS_VAR_GAMEID the Retro Achievements game ID, or 0 if not found
         *************************************************************************/
   CORO_SUB(RCHEEVOS_FILENAME_MD5)
      if (!string_is_empty(coro->path))
      {
         char base_noext[PATH_MAX_LENGTH];
         fill_pathname_base_noext(base_noext, coro->path, sizeof(base_noext));

         MD5_Init(&coro->md5);
         MD5_Update(&coro->md5, (void*)base_noext, strlen(base_noext));
         MD5_Final(coro->hash, &coro->md5);

         CORO_GOTO(RCHEEVOS_GET_GAMEID);
      }
      CORO_RET();

      /**************************************************************************
       * Info    Evaluates the CHEEVOS_VAR_MD5 hash
         * Inputs  CHEEVOS_VAR_INFO, CHEEVOS_VAR_OFFSET, CHEEVOS_VAR_COUNT
         * Outputs CHEEVOS_VAR_MD5, CHEEVOS_VAR_COUNT
         *************************************************************************/
   CORO_SUB(RCHEEVOS_EVAL_MD5)

      if (coro->count == 0)
         coro->count = coro->len;

      if (coro->len - coro->offset < coro->count)
         coro->count = coro->len - coro->offset;

      /* size limit */
      if (coro->count > CHEEVOS_MB(64))
         coro->count = CHEEVOS_MB(64);

      MD5_Update(&coro->md5,
            (void*)((uint8_t*)coro->data + coro->offset),
            coro->count);
      CORO_RET();

      /**************************************************************************
       * Info    Updates the CHEEVOS_VAR_MD5 hash with a repeated value
         * Inputs  CHEEVOS_VAR_OFFSET, CHEEVOS_VAR_COUNT
         * Outputs CHEEVOS_VAR_MD5
         *************************************************************************/
   CORO_SUB(RCHEEVOS_FILL_MD5)

      {
         char buffer[4096];

         while (coro->count > 0)
         {
            size_t len = sizeof(buffer);

            if (len > coro->count)
               len = coro->count;

            memset((void*)buffer, coro->offset, len);
            MD5_Update(&coro->md5, (void*)buffer, len);
            coro->count -= len;
         }
      }

      CORO_RET();

      /**************************************************************************
       * Info    Gets the achievements from Retro Achievements
         * Inputs  coro->hash
         * Outputs CHEEVOS_VAR_GAMEID
         *************************************************************************/
   CORO_SUB(RCHEEVOS_GET_GAMEID)

      {
         int size;
         size = rc_url_get_gameid(coro->url, sizeof(coro->url), coro->hash);

         if (size < 0)
         {
            CHEEVOS_ERR(RCHEEVOS_TAG "buffer too small to create URL\n");
            CORO_RET();
         }

         rcheevos_log_url(RCHEEVOS_TAG "rc_url_get_gameid: %s\n", coro->url);
         CORO_GOSUB(RCHEEVOS_HTTP_GET);

         if (!coro->json)
            CORO_RET();

         coro->gameid = chevos_get_gameid(coro->json);

         CHEEVOS_FREE(coro->json);
         CHEEVOS_LOG(RCHEEVOS_TAG "got game id %u\n", coro->gameid);
         CORO_RET();
      }

      /**************************************************************************
       * Info    Gets the achievements from Retro Achievements
         * Inputs  CHEEVOS_VAR_GAMEID
         * Outputs CHEEVOS_VAR_JSON
         *************************************************************************/
   CORO_SUB(RCHEEVOS_GET_CHEEVOS)
   {
      int ret;

      CORO_GOSUB(RCHEEVOS_LOGIN);

      ret = rc_url_get_patch(coro->url, sizeof(coro->url), coro->settings->arrays.cheevos_username, rcheevos_locals.token, coro->gameid);

      if (ret < 0)
      {
         CHEEVOS_ERR(RCHEEVOS_TAG "buffer too small to create URL\n");
         CORO_STOP();
      }

      rcheevos_log_url(RCHEEVOS_TAG "rc_url_get_patch: %s\n", coro->url);
      CORO_GOSUB(RCHEEVOS_HTTP_GET);

      if (!coro->json)
      {
         CHEEVOS_ERR(RCHEEVOS_TAG "error getting achievements for game id %u\n", coro->gameid);
         CORO_STOP();
      }

      CHEEVOS_LOG(RCHEEVOS_TAG "got achievements for game id %u\n", coro->gameid);
      CORO_RET();
   }

      /**************************************************************************
       * Info    Gets the achievements from Retro Achievements
         * Inputs  CHEEVOS_VAR_GAMEID
         * Outputs CHEEVOS_VAR_JSON
         *************************************************************************/
   CORO_SUB(RCHEEVOS_GET_BADGES)

      badges_ctx = new_badges_ctx;

#ifdef HAVE_MENU_WIDGETS
      if (false) /* we always want badges if menu widgets are enabled */
#endif
      {
         settings_t *settings = config_get_ptr();
         if (!(
               string_is_equal(settings->arrays.menu_driver, "xmb") ||
               string_is_equal(settings->arrays.menu_driver, "ozone")
            ) ||
               !settings->bools.cheevos_badges_enable)
            CORO_RET();
      }

      for (coro->i = 0; coro->i < 2; coro->i++)
      {
         if (coro->i == 0)
         {
            coro->cheevo     = rcheevos_locals.core;
            coro->cheevo_end = coro->cheevo + rcheevos_locals.patchdata.core_count;
         }
         else
         {
            coro->cheevo     = rcheevos_locals.unofficial;
            coro->cheevo_end = coro->cheevo + rcheevos_locals.patchdata.unofficial_count;
         }

         for (; coro->cheevo < coro->cheevo_end; coro->cheevo++)
         {
            for (coro->j = 0 ; coro->j < 2; coro->j++)
            {
               coro->badge_fullpath[0] = '\0';
               fill_pathname_application_special(
                     coro->badge_fullpath,
                     sizeof(coro->badge_fullpath),
                     APPLICATION_SPECIAL_DIRECTORY_THUMBNAILS_CHEEVOS_BADGES);

               if (!path_is_directory(coro->badge_fullpath))
                  path_mkdir(coro->badge_fullpath);
               CORO_YIELD();
               if (coro->j == 0)
                  snprintf(coro->badge_name,
                        sizeof(coro->badge_name),
                        "%s.png", coro->cheevo->info->badge);
               else
                  snprintf(coro->badge_name,
                        sizeof(coro->badge_name),
                        "%s_lock.png", coro->cheevo->info->badge);

               fill_pathname_join(
                     coro->badge_fullpath,
                     coro->badge_fullpath,
                     coro->badge_name,
                     sizeof(coro->badge_fullpath));

               if (!badge_exists(coro->badge_fullpath))
               {
#ifdef CHEEVOS_LOG_BADGES
                  CHEEVOS_LOG(
                        RCHEEVOS_TAG "downloading badge %s\n",
                        coro->badge_fullpath);
#endif
                  snprintf(coro->url,
                        sizeof(coro->url),
                        "http://i.retroachievements.org/Badge/%s",
                        coro->badge_name);

                  CORO_GOSUB(RCHEEVOS_HTTP_GET);

                  if (coro->json)
                  {
                     if (!filestream_write_file(coro->badge_fullpath,
                              coro->json, coro->k))
                        CHEEVOS_ERR(RCHEEVOS_TAG "error writing badge %s\n", coro->badge_fullpath);
                     else
                     {
                        CHEEVOS_FREE(coro->json);
                        coro->json = NULL;
                     }
                  }
               }
            }
         }
      }

      CORO_RET();

      /**************************************************************************
       * Info Logs in the user at Retro Achievements
         *************************************************************************/
   CORO_SUB(RCHEEVOS_LOGIN)
   {
      const char* username;
      const char* password;
      const char* token;
      int ret;
      char tok[256];

      username = coro->settings->arrays.cheevos_username;
      password = coro->settings->arrays.cheevos_password;
      token    = coro->settings->arrays.cheevos_token;

      if (rcheevos_locals.token[0])
         CORO_RET();

      if (string_is_empty(username))
      {
         runloop_msg_queue_push(
               "Missing RetroAchievements account information.",
               0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         runloop_msg_queue_push(
               "Please fill in your account information in Settings.",
               0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         CHEEVOS_ERR(RCHEEVOS_TAG "login info not informed\n");
         CORO_STOP();
      }

      if (string_is_empty(token))
         ret = rc_url_login_with_password(coro->url, sizeof(coro->url),
               username, password);
      else
         ret = rc_url_login_with_token(coro->url, sizeof(coro->url),
               username, token);

      if (ret < 0)
      {
         CHEEVOS_ERR(RCHEEVOS_TAG "buffer too small to create URL\n");
         CORO_STOP();
      }

      rcheevos_log_url(RCHEEVOS_TAG "rc_url_login_with_password: %s\n", coro->url);
      CORO_GOSUB(RCHEEVOS_HTTP_GET);

      if (!coro->json)
      {
         runloop_msg_queue_push("RetroAchievements: Error contacting server.", 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         CHEEVOS_ERR(RCHEEVOS_TAG "error getting user token\n");

         CORO_STOP();
      }

      ret = rcheevos_get_token(coro->json, tok, sizeof(tok));

      if (ret != 0)
      {
         char msg[512];
         snprintf(msg, sizeof(msg),
               "RetroAchievements: %s",
               tok);
         runloop_msg_queue_push(msg, 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         *coro->settings->arrays.cheevos_token = 0;

         CHEEVOS_FREE(coro->json);
         CORO_STOP();
      }

      CHEEVOS_FREE(coro->json);

      if (coro->settings->bools.cheevos_verbose_enable)
      {
         char msg[256];
         snprintf(msg, sizeof(msg),
               "RetroAchievements: Logged in as \"%s\".",
               coro->settings->arrays.cheevos_username);
         msg[sizeof(msg) - 1] = 0;
         runloop_msg_queue_push(msg, 0, 3 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
      }

      strlcpy(rcheevos_locals.token, tok,
            sizeof(rcheevos_locals.token));

      /* Save token to config and clear pass on success */
      strlcpy(coro->settings->arrays.cheevos_token, tok,
            sizeof(coro->settings->arrays.cheevos_token));

      *coro->settings->arrays.cheevos_password = 0;
      CORO_RET();
   }

      /**************************************************************************
       * Info    Pauses execution for five seconds
         *************************************************************************/
   CORO_SUB(RCHEEVOS_DELAY)

      {
         retro_time_t t1;
         coro->t0         = cpu_features_get_time_usec();

         do
         {
            CORO_YIELD();
            t1 = cpu_features_get_time_usec();
         }while ((t1 - coro->t0) < 3000000);
      }

      CORO_RET();

      /**************************************************************************
       * Info    Makes a HTTP GET request
         * Inputs  CHEEVOS_VAR_URL
         * Outputs CHEEVOS_VAR_JSON
         *************************************************************************/
   CORO_SUB(RCHEEVOS_HTTP_GET)

      for (coro->k = 0; coro->k < 5; coro->k++)
      {
         if (coro->k != 0)
            CHEEVOS_LOG(RCHEEVOS_TAG "Retrying HTTP request: %u of 5\n", coro->k + 1);

         coro->json       = NULL;
         coro->conn       = net_http_connection_new(
               coro->url, "GET", NULL);

         if (!coro->conn)
         {
            CORO_GOSUB(RCHEEVOS_DELAY);
            continue;
         }

         /* Don't bother with timeouts here, it's just a string scan. */
         while (!net_http_connection_iterate(coro->conn)) {}

         /* Error finishing the connection descriptor. */
         if (!net_http_connection_done(coro->conn))
         {
            net_http_connection_free(coro->conn);
            continue;
         }

         coro->http = net_http_new(coro->conn);

         /* Error connecting to the endpoint. */
         if (!coro->http)
         {
            net_http_connection_free(coro->conn);
            CORO_GOSUB(RCHEEVOS_DELAY);
            continue;
         }

         while (!net_http_update(coro->http, NULL, NULL))
            CORO_YIELD();

         {
            size_t length;
            uint8_t *data = net_http_data(coro->http,
                  &length, false);

            if (data)
            {
               coro->json = (char*)malloc(length + 1);

               if (coro->json)
               {
                  memcpy((void*)coro->json, (void*)data, length);
                  CHEEVOS_FREE(data);
                  coro->json[length] = 0;
               }

               coro->k = (unsigned)length;
               net_http_delete(coro->http);
               net_http_connection_free(coro->conn);
               CORO_RET();
            }
         }

         net_http_delete(coro->http);
         net_http_connection_free(coro->conn);
      }

      CHEEVOS_LOG(RCHEEVOS_TAG "Couldn't connect to server after 5 tries\n");
      CORO_RET();

      /**************************************************************************
       * Info    Deactivates the achievements already awarded
       * Inputs  CHEEVOS_VAR_GAMEID
       * Outputs
       *************************************************************************/
   CORO_SUB(RCHEEVOS_DEACTIVATE)

      CORO_GOSUB(RCHEEVOS_LOGIN);
      {
         int ret;
         unsigned mode;

         for (coro->i = 0; coro->i < 2; coro->i++)
         {
            ret = rc_url_get_unlock_list(coro->url, sizeof(coro->url), coro->settings->arrays.cheevos_username, rcheevos_locals.token, coro->gameid, coro->i);

            if (ret < 0)
            {
               CHEEVOS_ERR(RCHEEVOS_TAG "buffer too small to create URL\n");
               CORO_STOP();
            }

            rcheevos_log_url(RCHEEVOS_TAG "rc_url_get_unlock_list: %s\n", coro->url);
            CORO_GOSUB(RCHEEVOS_HTTP_GET);

            if (coro->json)
            {
               mode = coro->i == 0 ? RCHEEVOS_ACTIVE_SOFTCORE : RCHEEVOS_ACTIVE_HARDCORE;
               rcheevos_deactivate_unlocks(coro->json, rcheevos_unlock_cb, &mode);
               CHEEVOS_FREE(coro->json);
            }
            else
               CHEEVOS_ERR(RCHEEVOS_TAG "error retrieving list of unlocked achievements in softcore mode\n");
         }
      }

      CORO_RET();

      /**************************************************************************
       * Info    Posts the "playing" activity to Retro Achievements
         * Inputs  CHEEVOS_VAR_GAMEID
         * Outputs
         *************************************************************************/
   CORO_SUB(RCHEEVOS_PLAYING)

      snprintf(
            coro->url, sizeof(coro->url),
            "http://retroachievements.org/dorequest.php?r=postactivity&u=%s&t=%s&a=3&m=%u",
            coro->settings->arrays.cheevos_username,
            rcheevos_locals.token, coro->gameid
            );

      coro->url[sizeof(coro->url) - 1] = 0;
      rcheevos_log_url(RCHEEVOS_TAG "url to post the 'playing' activity: %s\n", coro->url);

      CORO_GOSUB(RCHEEVOS_HTTP_GET);

      if (coro->json)
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "posted playing activity\n");
         CHEEVOS_FREE(coro->json);
      }
      else
         CHEEVOS_ERR(RCHEEVOS_TAG "error posting playing activity\n");

      CHEEVOS_LOG(RCHEEVOS_TAG "posted playing activity\n");
      CORO_RET();

   CORO_LEAVE();
}

static void rcheevos_task_handler(retro_task_t *task)
{
   rcheevos_coro_t *coro = (rcheevos_coro_t*)task->state;

   if (!coro)
      return;

   if (!rcheevos_iterate(coro) || task_get_cancelled(task))
   {
      task_set_finished(task, true);

      CHEEVOS_LOCK(rcheevos_locals.task_lock);
      rcheevos_locals.task = NULL;
      CHEEVOS_UNLOCK(rcheevos_locals.task_lock);

      if (task_get_cancelled(task))
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Load task cancelled\n");
      }
      else
      {
         CHEEVOS_LOG(RCHEEVOS_TAG "Load task finished\n");
      }

      CHEEVOS_FREE(coro->data);
      CHEEVOS_FREE(coro->path);
      CHEEVOS_FREE(coro);
   }
}

bool rcheevos_load(const void *data)
{
   retro_task_t *task;
   const struct retro_game_info *info = NULL;
   rcheevos_coro_t *coro                       = NULL;

   rcheevos_loaded = false;
   rcheevos_hardcore_paused = false;

   if (!rcheevos_locals.core_supports || !data)
      return false;

   coro = (rcheevos_coro_t*)calloc(1, sizeof(*coro));

   if (!coro)
      return false;

   task = task_init();

   if (!task)
   {
      CHEEVOS_FREE(coro);
      return false;
   }

   CORO_SETUP();

   info = (const struct retro_game_info*)data;

   if (info->data)
   {
      coro->len = info->size;

      /* size limit */
      if (coro->len > CHEEVOS_MB(64))
         coro->len = CHEEVOS_MB(64);

      coro->data = malloc(coro->len);

      if (!coro->data)
      {
         CHEEVOS_FREE(task);
         CHEEVOS_FREE(coro);
         return false;
      }

      memcpy(coro->data, info->data, coro->len);
      coro->path       = NULL;
   }
   else
   {
      coro->data       = NULL;
      coro->path       = strdup(info->path);
   }

   task->handler   = rcheevos_task_handler;
   task->state     = (void*)coro;
   task->mute      = true;
   task->callback  = NULL;
   task->user_data = NULL;
   task->progress  = 0;
   task->title     = NULL;

#ifdef HAVE_THREADS
   if (rcheevos_locals.task_lock == NULL)
   {
      rcheevos_locals.task_lock = slock_new();
   }
#endif

   CHEEVOS_LOCK(rcheevos_locals.task_lock);
   rcheevos_locals.task = task;
   CHEEVOS_UNLOCK(rcheevos_locals.task_lock);

   task_queue_push(task);

   return true;
}
