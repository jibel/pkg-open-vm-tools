/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * stateMachine.c --
 *
 * Implements a generic state machine for executing backup operations
 * asynchronously. Since VSS is based on an asynchronous poolling model,
 * we're basing all backup operations on a similar model controlled by this
 * state machine, even if it would be more eficient to use an event-driven
 * approach in some cases.
 *
 * Overall order of execution for when no errors occur:
 *
 * Start -> OnFreeze -> run sync provider -> OnThaw -> Finalize
 *
 * The sync provider state machine depends on the particular implementation.
 * For the sync driver, it enables the driver and waits for a "snapshot done"
 * message before finishing. For the VSS subsystem, the sync provider just
 * implements a VSS backup cycle.
 */

#include "vmBackup.h"
#include "vmBackupInt.h"
#include "vmbackup_def.h"

#include <errno.h>
#include <string.h>

#include "vm_basic_defs.h"
#include "vm_assert.h"

#include "debug.h"
#include "eventManager.h"
#include "posix.h"
#include "file.h"
#include "fileIO.h"
#include "guestApp.h"
#include "rpcin.h"
#include "rpcout.h"
#include "str.h"
#include "strutil.h"
#include "unicode.h"
#include "util.h"
#include "vmstdio.h"

typedef enum {
   VMBACKUP_SUCCESS = 0,
   VMBACKUP_INVALID_STATE,
   VMBACKUP_SCRIPT_ERROR,
   VMBACKUP_SYNC_ERROR,
   VMBACKUP_REMOTE_ABORT,
   VMBACKUP_UNEXPECTED_ERROR
} VmBackupStatus;


#define VMBACKUP_ENQUEUE_EVENT() {                                      \
   gBackupState->timerEvent = EventManager_Add(gEventQueue,             \
                                               gBackupState->pollPeriod,\
                                               VmBackupAsyncCallback,   \
                                               NULL);                   \
   ASSERT_MEM_ALLOC(gBackupState->timerEvent);                          \
}

static DblLnkLst_Links *gEventQueue = NULL;
static VmBackupState *gBackupState = NULL;
static VmBackupSyncProvider *gSyncProvider = NULL;


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackupKeepAliveCallback --
 *
 *    Sends a keep alive backup event to the VMX.
 *
 * Result
 *    TRUE.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
VmBackupKeepAliveCallback(void *clientData)   // IN
{
   ASSERT(gBackupState != NULL);
   gBackupState->keepAlive = NULL;
   gBackupState->SendEvent(VMBACKUP_EVENT_KEEP_ALIVE, 0, "");
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VmBackupReadConfig --
 *
 *    Reads the vmbackup config file. This file should contain the names of
 *    resources that will not be quiesced during the backup (for example,
 *    paths to be ignored by the sync driver or writers to be ignored by
 *    VSS). Each non-empty line not starting with a '#' character is
 *    considered an entry.
 *
 *    The contents are stored in an array in the backup state structure.
 *    The data is expected to be in UTF-8 format.
 *
 *    Note: currently, only the VSS subsystem uses this data.
 *
 * Results:
 *    TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
VmBackupReadConfig(VmBackupState *state)  // OUT
{
   Bool ret = TRUE;
   char *configDir;
   char *cfgPath;

   ASSERT(state != NULL);

   configDir = GuestApp_GetConfPath();
   if (configDir == NULL) {
      return FALSE;
   }

   cfgPath = Str_Asprintf(NULL, "%s%c%s", configDir, DIRSEPC, "vmbackup.conf");
   free(configDir);
   if (cfgPath == NULL) {
      return FALSE;
   }

   if (File_IsFile(cfgPath)) {
      FILE *file;
      StdIO_Status status;

      file = Posix_Fopen(cfgPath, "r");
      if (file == NULL) {
         Debug("Can't open cfg file: %s\n", strerror(errno));
         ret = FALSE;
         goto exit;
      }

      while (TRUE) {
         Bool skip = TRUE;
         char *line;
         char *c;

         status = StdIO_ReadNextLine(file, &line, 0, NULL);
         if (status != StdIO_Success) {
            break;
         }

         /* Check if the line is empty or a comment. */
         for (c = line; *c != '\0'; c++) {
            if (*c != ' ' && *c != '\t') {
               skip = (*c == '#');
               break;
            }
         }

         if (!skip && !TargetArray_Push(&state->disabledTargets, line)) {
            free(line);
            break;
         }
      }

      ret = (ret && (status == StdIO_EOF));
      fclose(file);
   }

exit:
   if (!ret) {
      size_t i;
      for (i = 0; i < TargetArray_Count(&state->disabledTargets); i++) {
         free(*TargetArray_AddressOf(&state->disabledTargets, i));
      }
      TargetArray_Destroy(&state->disabledTargets);
      TargetArray_Init(&gBackupState->disabledTargets, 0);
   }
   return ret;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VmBackupSendEvent --
 *
 *    Sends a command to the VMX asking it to update VMDB about a new
 *    backup event.
 *
 * Result
 *    Whether sending the message succeeded.
 *
 * Side effects:
 *    Restarts the keep alive timer.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
VmBackupSendEvent(const char *event,   // IN: event name
                  const uint32 code,   // IN: result code
                  const char *desc)    // IN: message related to the code
{
   Bool success;
   ASSERT(gBackupState != NULL);

   if (gBackupState->keepAlive != NULL) {
      EventManager_Remove(gBackupState->keepAlive);
   }

   success = RpcOut_sendOne(NULL, NULL,
                            VMBACKUP_PROTOCOL_EVENT_SET" %s %u %s",
                            event, code, desc);

   if (!success) {
      Debug("VmBackup: failed to send event to the VMX.\n");
   }

   gBackupState->keepAlive = EventManager_Add(gEventQueue,
                                              VMBACKUP_KEEP_ALIVE_PERIOD / 20,
                                              VmBackupKeepAliveCallback,
                                              NULL);
   ASSERT_MEM_ALLOC(gBackupState->keepAlive);
   return success;
}


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackupFinalize --
 *
 *    Cleans up the backup state object and sends a "done" event to
 *    the VMX.
 *
 * Result
 *    None.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static void
VmBackupFinalize(void)
{
   size_t i;

   ASSERT(gBackupState != NULL);
   Debug("*** %s\n", __FUNCTION__);

   if (gBackupState->currentOp != NULL) {
      VmBackup_Cancel(gBackupState->currentOp);
      VmBackup_Release(gBackupState->currentOp);
   }

   gBackupState->SendEvent(VMBACKUP_EVENT_REQUESTOR_DONE, VMBACKUP_SUCCESS, "");

   if (gBackupState->timerEvent != NULL) {
      EventManager_Remove(gBackupState->timerEvent);
   }

   if (gBackupState->keepAlive != NULL) {
      EventManager_Remove(gBackupState->keepAlive);
   }

   for (i = 0; i < TargetArray_Count(&gBackupState->disabledTargets); i++) {
      free(*TargetArray_AddressOf(&gBackupState->disabledTargets, i));
   }
   TargetArray_Destroy(&gBackupState->disabledTargets);

   free(gBackupState->volumes);
   free(gBackupState);
   gBackupState = NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackupStartScripts --
 *
 *    Starts the execution of the scripts for the given action type.
 *
 * Result
 *    TRUE, unless starting the scripts fails for some reason.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
VmBackupStartScripts(VmBackupScriptType type,   // IN
                     VmBackupCallback callback) // IN
{
   const char *opName;
   Debug("*** %s\n", __FUNCTION__);

   switch (type) {
      case VMBACKUP_SCRIPT_FREEZE:
         opName = "VmBackupOnFreeze";
         break;

      case VMBACKUP_SCRIPT_FREEZE_FAIL:
         opName = "VmBackupOnFreezeFail";
         break;

      case VMBACKUP_SCRIPT_THAW:
         opName = "VmBackupOnThaw";
         break;

      default:
         NOT_REACHED();
   }

   if (!VmBackup_SetCurrentOp(gBackupState,
                              VmBackupNewScriptOp(type, gBackupState),
                              callback,
                              opName)) {
      gBackupState->SendEvent(VMBACKUP_EVENT_REQUESTOR_ERROR,
                              VMBACKUP_SCRIPT_ERROR,
                              "Error when starting backup scripts.");
      return FALSE;
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackupAsyncCallback --
 *
 *    Callback for the event manager. Checks the status of the current
 *    async operation being monitored, and calls the queued operations
 *    as needed.
 *
 * Result
 *    TRUE.
 *
 * Side effects:
 *    Several, depending on the backup state.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
VmBackupAsyncCallback(void *clientData)   // IN
{
   Bool finalize = FALSE;

   ASSERT(gBackupState != NULL);
   Debug("*** %s\n", __FUNCTION__);

   gBackupState->timerEvent = NULL;

   if (gBackupState->currentOp != NULL) {
      VmBackupOpStatus status;

      Debug("VmBackupAsyncCallback: checking %s\n", gBackupState->currentOpName);
      status = VmBackup_QueryStatus(gBackupState->currentOp);

      switch (status) {
      case VMBACKUP_STATUS_PENDING:
         goto exit;

      case VMBACKUP_STATUS_FINISHED:
         Debug("Async request completed\n");
         VmBackup_Release(gBackupState->currentOp);
         gBackupState->currentOp = NULL;
         break;

      default:
         {
            Bool freeMsg = TRUE;
            char *errMsg = Str_Asprintf(NULL,
                                        "Asynchronous operation failed: %s\n",
                                        gBackupState->currentOpName);
            if (errMsg == NULL) {
               freeMsg = FALSE;
               errMsg = "Asynchronous operation failed.";
            }
            gBackupState->SendEvent(VMBACKUP_EVENT_REQUESTOR_ERROR,
                                    VMBACKUP_UNEXPECTED_ERROR,
                                    errMsg);
            if (freeMsg) {
               free(errMsg);
            }

            VmBackup_Release(gBackupState->currentOp);
            gBackupState->currentOp = NULL;

            /*
             * If we get an error when running the freeze scripts, we want to
             * schedule the "fail" scripts to run.
             */
            if (!gBackupState->syncProviderRunning &&
                gBackupState->scripts != NULL) {
               gBackupState->callback = NULL;
               finalize = !VmBackupStartScripts(VMBACKUP_SCRIPT_FREEZE_FAIL, NULL);
            }
            goto exit;
         }
      }
   }

   /*
    * Keep calling the registered callback until it's either NULL, or
    * an asynchronous operation is scheduled.
    */
   while (gBackupState->callback != NULL) {
      Bool cbRet;
      VmBackupCallback cb = gBackupState->callback;
      gBackupState->callback = NULL;

      cbRet = cb(gBackupState);
      if (cbRet) {
         if (gBackupState->currentOp != NULL || gBackupState->forceRequeue) {
            goto exit;
         }
      } else {
         /*
          * Finalize the backup operation, unless the sync provider is still
          * active. In which case, delay finalization until after the sync
          * provider is finished cleaning up after itself.
          */
         finalize = gBackupState->syncProviderFailed ||
                    !gBackupState->syncProviderRunning;
         gBackupState->syncProviderFailed = gBackupState->syncProviderRunning;
      }
   }

   /*
    * If the sync provider is currently in execution and there's no
    * callback set, that means the sync provider is done executing,
    * so run the thaw scripts if we've received a "snapshot done"
    * event.
    */
   if (gBackupState->syncProviderRunning &&
       (gBackupState->snapshotDone ||
        gBackupState->syncProviderFailed ||
        gBackupState->clientAborted) &&
       gBackupState->callback == NULL) {
      gBackupState->syncProviderRunning = FALSE;
      gBackupState->pollPeriod = 100;
      if (gBackupState->syncProviderFailed || gBackupState->clientAborted) {
         finalize = !VmBackupStartScripts(VMBACKUP_SCRIPT_FREEZE_FAIL, NULL);
      } else {
         finalize = !VmBackupStartScripts(VMBACKUP_SCRIPT_THAW, NULL);
      }
      goto exit;
   }

   /*
    * If the sync provider is not running anymore, and either the operation
    * was aborted by the remote client or we don't have any callbacks to
    * process anymore, it must mean we're finished.
    */
   finalize = (!gBackupState->syncProviderRunning &&
               (gBackupState->callback == NULL || gBackupState->clientAborted));

exit:
   if (finalize) {
      VmBackupFinalize();
   } else {
      gBackupState->forceRequeue = FALSE;
      VMBACKUP_ENQUEUE_EVENT();
   }
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackupEnableSync --
 *
 *    Calls the sync provider's start function.
 *
 * Result
 *    Whether the sync provider call succeeded.
 *
 * Side effects:
 *    Depends on the sync provider.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
VmBackupEnableSync(VmBackupState *state)
{
   ASSERT(state != NULL);
   Debug("*** %s\n", __FUNCTION__);
   if (!gSyncProvider->start(state, gSyncProvider->clientData)) {
      state->SendEvent(VMBACKUP_EVENT_REQUESTOR_ERROR,
                       VMBACKUP_SYNC_ERROR,
                       "Error when enabling the sync provider.");
      return FALSE;
   }

   state->syncProviderRunning = TRUE;
   return TRUE;
}


/* RpcIn callbacks. */


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackupStart --
 *
 *    Handler for the "vmbackup.start" message. Starts the "freeze" scripts
 *    unless there's another backup operation going on or some other
 *    unexpected error occurs.
 *
 * Result
 *    TRUE, unless an error occurs.
 *
 * Side effects:
 *    Depends on what the scripts do.
 *
 *-----------------------------------------------------------------------------
 */

Bool
VmBackupStart(char const **result,     // OUT
              size_t *resultLen,       // OUT
              const char *name,        // IN
              const char *args,        // IN
              size_t argsSize,         // IN
              void *clientData)        // IN
{
   Debug("*** %s\n", __FUNCTION__);
   if (gBackupState != NULL) {
      return RpcIn_SetRetVals(result,
                              resultLen,
                              "Backup operation already in progress.",
                              FALSE);
   }

   gBackupState = Util_SafeMalloc(sizeof *gBackupState);
   memset(gBackupState, 0, sizeof *gBackupState);

   gBackupState->SendEvent = VmBackupSendEvent;
   gBackupState->pollPeriod = 100;
   TargetArray_Init(&gBackupState->disabledTargets, 0);

   if (argsSize > 0) {
      int generateManifests = 0;
      int index = 0;

      if (StrUtil_GetNextIntToken(&generateManifests, &index, args, " ")) {
         gBackupState->generateManifests = generateManifests;
      }

      if (args[index] != '\0') {
         gBackupState->volumes = Util_SafeStrdup(args + index);
      }
   }

   if (!VmBackupReadConfig(gBackupState)) {
      free(gBackupState);
      gBackupState = NULL;
      return RpcIn_SetRetVals(result,
                              resultLen,
                              "Error when reading configuration file.",
                              FALSE);
   }

   gBackupState->SendEvent(VMBACKUP_EVENT_RESET, VMBACKUP_SUCCESS, "");

   if (!VmBackupStartScripts(VMBACKUP_SCRIPT_FREEZE, VmBackupEnableSync)) {
      free(gBackupState);
      gBackupState = NULL;
      return RpcIn_SetRetVals(result,
                              resultLen,
                              "Error initializing backup.",
                              FALSE);
   }

   VMBACKUP_ENQUEUE_EVENT();
   return RpcIn_SetRetVals(result, resultLen, "", TRUE);
}


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackupAbort --
 *
 *    Aborts the current operation if one is active, and stops the backup
 *    process. If the sync provider has been activated, tell it to abort
 *    the ongoing operation.
 *
 * Result
 *    TRUE
 *
 * Side effects:
 *    Possibly many.
 *
 *-----------------------------------------------------------------------------
 */

Bool
VmBackupAbort(char const **result,     // OUT
              size_t *resultLen,       // OUT
              const char *name,        // IN
              const char *args,        // IN
              size_t argsSize,         // IN
              void *clientData)        // IN
{
   if (gBackupState != NULL) {
      Debug("*** %s\n", __FUNCTION__);

      if (gBackupState->currentOp != NULL) {
         VmBackup_Cancel(gBackupState->currentOp);
         VmBackup_Release(gBackupState->currentOp);
         gBackupState->currentOp = NULL;
      }

      if (gBackupState->syncProviderRunning) {
         gSyncProvider->abort(gBackupState, gSyncProvider->clientData);
      }

      gBackupState->clientAborted = TRUE;
      gBackupState->SendEvent(VMBACKUP_EVENT_REQUESTOR_ABORT,
                              VMBACKUP_REMOTE_ABORT,
                              "Remote abort.");

      return RpcIn_SetRetVals(result, resultLen, "", TRUE);
   } else {
      return RpcIn_SetRetVals(result, resultLen,
                              "Error: no backup in progress", FALSE);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackupSnapshotDone --
 *
 *    Sets the flag that says it's OK to disable the sync driver.
 *
 * Result
 *    TRUE
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
VmBackupSnapshotDone(char const **result,    // OUT
                     size_t *resultLen,      // OUT
                     const char *name,       // IN
                     const char *args,       // IN
                     size_t argsSize,        // IN
                     void *clientData)       // IN
{
   if (gBackupState != NULL) {
      Debug("*** %s\n", __FUNCTION__);
      if (!gSyncProvider->snapshotDone(gBackupState, gSyncProvider->clientData)) {
         gBackupState->syncProviderFailed = TRUE;
         gBackupState->SendEvent(VMBACKUP_EVENT_REQUESTOR_ERROR,
                                 VMBACKUP_SYNC_ERROR,
                                 "Error when notifying the sync provider.");
      } else {
         gBackupState->snapshotDone = TRUE;
      }
      return RpcIn_SetRetVals(result, resultLen, "", TRUE);
   } else {
      return RpcIn_SetRetVals(result, resultLen,
                              "Error: no backup in progress", FALSE);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackup_Init --
 *
 *    Registers the RpcIn callbacks for the backup protocol.
 *
 * Result
 *    TRUE.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
VmBackup_Init(RpcIn *rpcin,                     // IN
              DblLnkLst_Links *eventQueue,      // IN
              VmBackupSyncProvider *provider)   // IN
{
   ASSERT(gEventQueue == NULL);
   ASSERT(eventQueue != NULL);
   ASSERT(provider != NULL);
   ASSERT(provider->start != NULL);
   ASSERT(provider->abort != NULL);
   ASSERT(provider->snapshotDone != NULL);
   ASSERT(provider->release != NULL);

   RpcIn_RegisterCallback(rpcin,
                          VMBACKUP_PROTOCOL_START,
                          VmBackupStart,
                          NULL);
   RpcIn_RegisterCallback(rpcin,
                          VMBACKUP_PROTOCOL_ABORT,
                          VmBackupAbort,
                          NULL);
   RpcIn_RegisterCallback(rpcin,
                          VMBACKUP_PROTOCOL_SNAPSHOT_DONE,
                          VmBackupSnapshotDone,
                          NULL);
   gEventQueue = eventQueue;
   gSyncProvider = provider;
   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 *  VmBackup_Shutdown --
 *
 *    Unregisters the RpcIn callbacks.
 *
 * Result
 *    None.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

void
VmBackup_Shutdown(RpcIn *rpcin)
{
   if (gBackupState != NULL) {
      VmBackupFinalize();
   }

   gSyncProvider->release(gSyncProvider);
   gSyncProvider = NULL;

   RpcIn_UnregisterCallback(rpcin, VMBACKUP_PROTOCOL_START);
   RpcIn_UnregisterCallback(rpcin, VMBACKUP_PROTOCOL_ABORT);
   RpcIn_UnregisterCallback(rpcin, VMBACKUP_PROTOCOL_SNAPSHOT_DONE);
   gEventQueue = NULL;
}

