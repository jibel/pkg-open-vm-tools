/*********************************************************
 * Copyright (C) 1998 VMware, Inc. All rights reserved.
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
 * printer.c --
 *
 *      This file implements assorted printer related functionality.
 *
 *      This library is currently only implemented for Win32, and uses
 *      Win32 API functions that are only available for Windows NT and
 *      later. However, this library is linked into code that runs on
 *      Win9x, and thus dynamically loads its Win32 API functions
 *      from DLL.
 *
 *      Therefore, users of this library must call Printer_Init()
 *      before calling anything else.
 */

#ifdef _WIN32
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "vmware.h"
#include "err.h"
#include "printer.h"

#define LOGLEVEL_MODULE printer
#include "loglevel_user.h"


/*
 * State needed for dynamically loading functions from DLLs.
 */

#ifdef _WIN32
HMODULE winspoolDll = NULL;

typedef BOOL (WINAPI *AddPrinterConnectionFunc)(LPTSTR);
typedef BOOL (WINAPI *SetDefaultPrinterFunc)(LPCTSTR);
typedef BOOL (WINAPI *GetDefaultPrinterFunc)(LPTSTR, LPDWORD);

AddPrinterConnectionFunc addPrinterConnectionFunc  = NULL;
GetDefaultPrinterFunc    getDefaultPrinterFunc     = NULL;
SetDefaultPrinterFunc    setDefaultPrinterFunc     = NULL;
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Printer_GetDefault --
 *
 *      Get the default system printer name. The returned string is
 *      allocated.
 *
 *      (Currently only implemented on Windows.)
 *
 * Results:
 *      Name of default printer
 *      NULL on failure (or if there is no default).
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

char *
Printer_GetDefault(void)
{
#ifdef _WIN32
   char *printerName = NULL;
   DWORD bufSize = 0;
   DWORD error;
   Bool success;

   /* Make sure function has been loaded. */
   if (!getDefaultPrinterFunc) {
      Log("Printer_GetDefault: DLL not loaded\n");
      ASSERT(FALSE);
      return NULL;
   }

   /* Find the necessary buffer size. */
   success = getDefaultPrinterFunc(printerName, &bufSize);
   error = Err_Errno();
   if (success) {
      Log("Printer_GetDefault: Didn't fail with zero buffer\n");
      return NULL;
   }

   if (error != ERROR_INSUFFICIENT_BUFFER) {
      Log("Printer_GetDefault: Unexpected failure %d: %s\n",
          error, Err_Errno2String(error));
      return NULL;
   }

   printerName = malloc(bufSize);
   if (!printerName) {
      Log("Printer_GetDefault: Memory allocation failure\n");
      return NULL;
   }

   success = getDefaultPrinterFunc(printerName, &bufSize);
   if (!success) {
      error = Err_Errno();
      Log("Printer_GetDefault: Failed to get default printer\n");
      free(printerName);
      return NULL;
   }

   return printerName;

#else
   NOT_IMPLEMENTED();
   return NULL;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Printer_SetDefault --
 *
 *      Set the default system printer name.
 *
 *      (Currently only implemented on Windows.)
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Printer_SetDefault(char const *printerName) // IN
{
#ifdef _WIN32
   DWORD error;

   /* Make sure function has been loaded. */
   if (!setDefaultPrinterFunc) {
      Log("Printer_SetDefault: DLL not loaded\n");
      ASSERT(FALSE);
      return FALSE;
   }

   /*
    * Attempt to set the default printer.
    */
   if (!setDefaultPrinterFunc(printerName)) {
      error = Err_Errno();
      Log("Printer_SetDefault: Unable to SetDefaultPrinter %d: %s\n",
          error, Err_Errno2String(error));
      return FALSE;
   }

   return TRUE;

#else
   NOT_IMPLEMENTED();
   return FALSE;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Printer_AddConnection --
 *
 *      Add a connection to the given printer for the current user.
 *      (Win32 only)
 *
 *      Printer connections are per-user, so this code must be run in
 *      a user's login session in order to work. (Error code 2 is
 *      returned otherwise, e.g. if this code is run from a service.)
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Printer_AddConnection(char *printerName, // IN:  Name of printer to add
                      int *sysError)     // OUT: System error code (errno)
{
#ifdef _WIN32
   DWORD error = ERROR_SUCCESS;
   Bool success = TRUE;

   ASSERT(printerName);
   ASSERT(sysError);

   /* Make sure function has been loaded. */
   if (!addPrinterConnectionFunc) {
      Log("Printer_AddConnection: DLL not loaded\n");
      ASSERT(FALSE);
      return FALSE;
   }

   /* Try to add the printer. */
   if (!addPrinterConnectionFunc((LPSTR)printerName)) {
      error = Err_Errno();
      Log("Printer_AddConnection: Failed to add printer %s : %d %s\n",
          printerName, error, Err_Errno2String(error));
      success = FALSE;
   }

   *sysError = error;
   return success;

#else
   NOT_IMPLEMENTED();
   return FALSE;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Printer_Init --
 *
 *      Load the libraries for needed Win32 API functions. This is
 *      necessary because this code is used in the Tools in Win9x guests,
 *      and Win9x lacks support for some of these functions, so we can't
 *      statically link them.
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Printer_Init(void)
{
#ifdef _WIN32
   DWORD error;

   /*
    * Try to load the necessary library.
    */
   winspoolDll = LoadLibrary("Winspool.drv");
   if (!winspoolDll) {
      error = Err_Errno();
      Log("Printer_Init: Failed to load Winspool.drv  %d: %s\n",
          error, Err_Errno2String(error));

      Log("Printer_Init: Trying to load Winspool as Winspool.dll...\n");
      winspoolDll = LoadLibrary("Winspool");
      if (!winspoolDll) {
         error = Err_Errno();
         Log("Printer_Init: Failed to load Winspool.dll  %d: %s\n",
             error, Err_Errno2String(error));
         Log("Unable to load Winspool, giving up.\n");
         return FALSE;
      }
   }

   /* Load individual functions. */
   getDefaultPrinterFunc = (GetDefaultPrinterFunc)GetProcAddress(winspoolDll,
                                                                 "GetDefaultPrinterA");
   if (!getDefaultPrinterFunc) {
      error = Err_Errno();
      Log("Printer_Init: Failed to load GetDefaultPrinter %d: %s\n",
          error, Err_Errno2String(error));
      goto error;
   }

   setDefaultPrinterFunc = (SetDefaultPrinterFunc)GetProcAddress(winspoolDll,
                                                                 "SetDefaultPrinterA");
   if (!setDefaultPrinterFunc) {
      error = Err_Errno();
      Log("Printer_Init: Failed to load SetDefaultPrinter %d: %s\n",
          error, Err_Errno2String(error));
      goto error;
   }

   addPrinterConnectionFunc = (AddPrinterConnectionFunc)GetProcAddress(winspoolDll,
                                                             "AddPrinterConnectionA");
   if (!addPrinterConnectionFunc) {
      error = Err_Errno();
      Log("Printer_Init: Failed to load AddPrinterConnection %d: %s\n",
          error, Err_Errno2String(error));
      goto error;
   }

   return TRUE;

  error:
   addPrinterConnectionFunc  = NULL;
   setDefaultPrinterFunc     = NULL;
   getDefaultPrinterFunc     = NULL;
   return FALSE;

#else
   /* Allow this to succeed for non-Win32. */
   return TRUE;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Printer_Cleanup --
 *
 *      Cleanup all the state loaded by Printer_Init().
 *
 * Results:
 *      TRUE on success
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Printer_Cleanup(void)
{
#ifdef _WIN32
   DWORD error;

   if (!winspoolDll) {
      Log("Printer_Cleanup: Printer library not loaded.\n");
      return FALSE;
   }

   addPrinterConnectionFunc  = NULL;
   setDefaultPrinterFunc     = NULL;
   getDefaultPrinterFunc     = NULL;

   if (!FreeLibrary(winspoolDll)) {
      error = Err_Errno();
      Log("Printer_Cleanup: Failed to FreeLibrary %d: %s\n",
          error, Err_Errno2String(error));
      winspoolDll = NULL;
      return FALSE;
   }

   winspoolDll = NULL;
   return TRUE;

#else
   /* Allow this to succeed for non-Win32. */
   return TRUE;
#endif
}
