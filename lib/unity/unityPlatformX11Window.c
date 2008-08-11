/*********************************************************
 * Copyright (C) 2007-2008 VMware, Inc. All rights reserved.
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
 * unityPlatformX11Window.c --
 *
 *    Implementation of Unity for guest operating systems that use the X11 windowing
 *    system. This file implements per-window operations (move, minimize, etc.)
 */

#include "unityX11.h"
#include "base64.h"
#include "region.h"
#include "imageUtil.h"
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include "Uri.h"
#include "appUtil.h"

/*
 * Utility routines
 */
static Bool UnityPlatformFindWindows(UnityPlatform *up,
                                     Window currentWindow,
                                     Window *toplevelWindow,
                                     Window *clientWindow,
                                     Window *rootWindow);
static Bool UPWindowPushFullUpdate(UnityPlatform *up, UnityPlatformWindow *upw);
static void UPWindowSetRelevance(UnityPlatform *up,
                                 UnityPlatformWindow *upw,
                                 Bool isRelevant);
static void UPWindowUpdateActions(UnityPlatform *up, UnityPlatformWindow *upw);
static void UPWindowUpdateDesktop(UnityPlatform *up, UnityPlatformWindow *upw);
static void UPWindowUpdateIcon(UnityPlatform *up, UnityPlatformWindow *upw);
static void UPWindowUpdateProtocols(UnityPlatform *up, UnityPlatformWindow *upw);
#if defined(VM_HAVE_X11_SHAPE_EXT)
static void UPWindowUpdateShape(UnityPlatform *up, UnityPlatformWindow *upw);
#endif
static void UPWindowUpdateState(UnityPlatform *up, UnityPlatformWindow *upw);
static void UPWindowUpdateTitle(UnityPlatform *up, UnityPlatformWindow *upw);
static void UPWindowUpdateType(UnityPlatform *up, UnityPlatformWindow *upw);
static void UPWindowProcessConfigureEvent(UnityPlatform *up,
                                          UnityPlatformWindow *upw,
                                          const XEvent *xevent);
static void UPWindowProcessPropertyEvent(UnityPlatform *up,
                                         UnityPlatformWindow *upw,
                                         const XEvent *xevent);
static void UPWindowProcessShapeEvent(UnityPlatform *up,
                                      UnityPlatformWindow *upw,
                                      const XEvent *xevent);
static Bool UPWindowGetDesktop(UnityPlatform *up,
                               UnityPlatformWindow *upw,
                               int *guestDesktop);
static void UPWindowSetWindows(UnityPlatform *up,
                               UnityPlatformWindow *upw,
                               Window toplevelWindow,
                               Window clientWindow);


/*
 *-----------------------------------------------------------------------------
 *
 * UnityPlatformFindClientWindow --
 *
 *      In X, the immediate children of the root window are almost always window manager
 *      windows that hold the app's windows. Sometimes we want to find the actual app's
 *      window to operate on. Given a random window ID, this function figures out which
 *      is which.
 *
 * Results:
 *      TRUE if successful, FALSE otherwise.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
UnityPlatformFindWindows(UnityPlatform *up,      // IN
                         Window currentWindow,   // IN
                         Window *toplevelWindow, // OUT
                         Window *clientWindow,   // OUT
                         Window *rootWindow)     // OUT
{
   Bool retval = FALSE;

   Window rootWin;
   Window parentWin;
   Window *children = NULL;
   unsigned int numChildren;

   Atom propertyType;
   int propertyFormat;
   unsigned long itemsReturned;
   unsigned long bytesRemaining;
   unsigned char *valueReturned = NULL;

   ASSERT(up);
   ASSERT(toplevelWindow);
   ASSERT(clientWindow);
   ASSERT(rootWindow);

   /* Check for the WM_STATE property on the window */
   UnityPlatformResetErrorCount(up);
   XGetWindowProperty(up->display, (Window)currentWindow, up->atoms.WM_STATE, 0,
                      1024, False, AnyPropertyType,
                      &propertyType, &propertyFormat, &itemsReturned,
                      &bytesRemaining, &valueReturned);
   XFree(valueReturned);
   if (UnityPlatformGetErrorCount(up)) {
      Debug("Retrieving WM_STATE failed\n");
      return FALSE;
   }

   XQueryTree(up->display, currentWindow, &rootWin, &parentWin,
              &children, &numChildren);
   if (UnityPlatformGetErrorCount(up)) {
      Debug("XQueryTree failed\n");
      return FALSE;
   }

   if (propertyType != None) {
      /*
       * If WM_STATE exists on this window, we were given a client window
       */
      *clientWindow = currentWindow;
      *rootWindow = rootWin;

      XFree(children);
      children = NULL;

      /*
       * This loop ensures that parentWin is the direct child of the root.
       *
       * XXX this will break for any window managers that use subwindows to implement
       * virtual desktops.
       */
      while (parentWin != rootWin) {
         currentWindow = parentWin;

         XQueryTree(up->display, currentWindow, &rootWin,
                    &parentWin, &children, &numChildren);
         XFree(children);
         children = NULL;
      }
      *toplevelWindow = currentWindow;

      retval = TRUE;
   } else if (parentWin == rootWin) {
      int i;
      GQueue *windowQueue;

      /*
       * Do a breadth-first search down the window tree to find the child that has the
       * WM_STATE property.
       */
      ASSERT(UnityPlatformIsRootWindow(up, rootWin));

      *toplevelWindow = currentWindow;
      *rootWindow = rootWin;
      *clientWindow = None;

      windowQueue = g_queue_new();

      while (numChildren || !g_queue_is_empty(windowQueue)) {
         Window childWindow;

         for (i = 0; i < numChildren; i++) {
            g_queue_push_tail(windowQueue, GUINT_TO_POINTER(children[i]));
         }
         XFree(children);
         children = NULL;

         childWindow = GPOINTER_TO_UINT(g_queue_pop_head(windowQueue));

         propertyType = None;
         valueReturned = NULL;
         itemsReturned = 0;
         XGetWindowProperty(up->display, childWindow, up->atoms.WM_STATE, 0,
                            1024, False, AnyPropertyType,
                            &propertyType, &propertyFormat, &itemsReturned,
                            &bytesRemaining, &valueReturned);
         XFree(valueReturned);

         if (UnityPlatformGetErrorCount(up)) {
            g_queue_free(windowQueue);
            Debug("Getting WM_STATE on a child failed\n");
            return FALSE;
         }

         if (itemsReturned) {
            *clientWindow = childWindow;
            break;
         }

         XQueryTree(up->display, childWindow, &rootWin,
                    &parentWin, &children, &numChildren);
         if (UnityPlatformGetErrorCount(up)) {
            g_queue_free(windowQueue);
            Debug("XQueryTree failed\n");
            return FALSE;
         }
      }

      g_queue_free(windowQueue);

      retval = TRUE;
   } /* else, retval is FALSE */

   XFree(children);

   if (retval && (*toplevelWindow == *rootWindow || *clientWindow == *rootWindow)) {
      Panic("Creating a UnityPlatformWindow of a root window is a big error\n");
   }

   return retval;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowSetWindows --
 *
 *      Updates the X11 windows that a UnityPlatformWindow represents. Used mainly if a
 *      window is created or reparented.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates internal Unity state.
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowSetWindows(UnityPlatform *up,        // IN
                   UnityPlatformWindow *upw, // IN
                   Window toplevelWindow,    // IN
                   Window clientWindow)      // IN
{
   Bool wasRelevant;

   ASSERT(up);
   ASSERT(upw);

   wasRelevant = upw->isRelevant;

   UPWindowSetRelevance(up, upw, FALSE);
   if (upw->toplevelWindow) {
      XSelectInput(up->display, upw->toplevelWindow, 0);
      HashTable_Delete(up->allWindows, GUINT_TO_POINTER(upw->toplevelWindow));
   }
   if (upw->clientWindow) {
      XSelectInput(up->display, upw->clientWindow, 0);
      HashTable_Delete(up->allWindows, GUINT_TO_POINTER(upw->clientWindow));
   }

   upw->toplevelWindow = toplevelWindow;
   upw->clientWindow = clientWindow;

   /*
    * Start listening to events on this window. We want these even if the window is of no
    * interest to us, because the specified events may make the window interesting to us.
    */
   if (clientWindow) {
      XSelectInput(up->display, clientWindow, PropertyChangeMask | StructureNotifyMask);
   }

   XSelectInput(up->display,
                toplevelWindow,
                FocusChangeMask | PropertyChangeMask | StructureNotifyMask);

#if defined(VM_HAVE_X11_SHAPE_EXT)
   if (up->shapeEventBase) {
      XShapeSelectInput(up->display, toplevelWindow, ShapeNotifyMask);
   }
#endif

   HashTable_Insert(up->allWindows, GUINT_TO_POINTER(upw->toplevelWindow), upw);
   if (upw->clientWindow) {
      HashTable_Insert(up->allWindows, GUINT_TO_POINTER(upw->clientWindow), upw);
   }
   UPWindowSetRelevance(up, upw, wasRelevant);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindow_Create --
 *
 *      Creates a UnityPlatformWindow for the specified UnityWindowId. The resulting
 *      object will have a reference count of 1 that is owned by the caller.
 *
 * Results:
 *      The newly created UnityPlatformWindow.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

UnityPlatformWindow *
UPWindow_Create(UnityPlatform *up,     // IN
                Window window)         // IN
{
   UnityPlatformWindow *upw;
   Window toplevelWindow;
   Window clientWindow;
   Window rootWindow;

   ASSERT(up);
   ASSERT(window != None);

   if (!UnityPlatformFindWindows(up, window,
                                 &toplevelWindow, &clientWindow, &rootWindow)) {
      Debug("FindWindows failed on %#lx\n", window);
      return NULL;
   }

   if (HashTable_Lookup(up->allWindows,
                        GUINT_TO_POINTER(toplevelWindow),
                        (void **)&upw)) {
      Debug("Lookup of window %#lx returned %#lx\n",
            toplevelWindow, upw->toplevelWindow);
      abort();
   }

   if (HashTable_Lookup(up->allWindows,
                        GUINT_TO_POINTER(clientWindow),
                        (void **)&upw)) {
      Debug("Lookup of clientWindow %#lx returned existing toplevel %#lx\n",
            clientWindow, upw->toplevelWindow);
      return NULL;
   }

   upw = Util_SafeCalloc(1, sizeof *upw);
   upw->refs = 1;

   Debug("Creating new window for %#lx/%#lx/%#lx\n",
         toplevelWindow, clientWindow, rootWindow);
   upw->rootWindow = rootWindow;
   for (upw->screenNumber = 0;
        upw->screenNumber < up->rootWindows->numWindows
        && up->rootWindows->windows[upw->screenNumber] != rootWindow;
        upw->screenNumber++);
   ASSERT (upw->screenNumber < up->rootWindows->numWindows);

   DynBuf_Init(&upw->iconPng.data);
   DynBuf_SetSize(&upw->iconPng.data, 0);

   UPWindowSetWindows(up, upw, toplevelWindow, clientWindow);

   /*
    * Put newly created windows on the top of the stack by default.
    */
   upw->higherWindow = NULL;
   upw->lowerWindow = up->topWindow;
   if (upw->lowerWindow) {
      upw->lowerWindow->higherWindow = upw;
   }
   up->topWindow = upw;

   return upw;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindow_Ref --
 *
 *      Increases the reference count on the UnityPlatformWindow object by one.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
UPWindow_Ref(UnityPlatform *up,        // IN
             UnityPlatformWindow *upw) // IN
{
   upw->refs++;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindow_Unref --
 *
 *      Decreases the reference count on the UnityPlatformWindow object by one.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May destroy the object if no references remain.
 *
 *-----------------------------------------------------------------------------
 */

void
UPWindow_Unref(UnityPlatform *up,        // IN
               UnityPlatformWindow *upw) // IN
{
   upw->refs--;

   if (upw->refs <= 0) { // Window needs destroying
      UPWindowSetRelevance(up, upw, FALSE);

      /*
       * Filter out windows that have been already destroyed on the X11 side, but which
       * still may have had refcounts active.
       */
      if (upw->windowType != UNITY_WINDOW_TYPE_NONE) {
         XSelectInput(up->display, upw->toplevelWindow, 0);

#if defined(VM_HAVE_X11_SHAPE_EXT)
         if (up->shapeEventBase) {
            XShapeSelectInput(up->display, upw->toplevelWindow, 0);
         }
#endif

         if (upw->clientWindow) {
            XSelectInput(up->display, upw->clientWindow, 0);
         }
      }

      HashTable_Delete(up->allWindows, GUINT_TO_POINTER(upw->toplevelWindow));
      if (upw->clientWindow) {
         HashTable_Delete(up->allWindows, GUINT_TO_POINTER(upw->clientWindow));
      }

      DynBuf_Destroy(&upw->iconPng.data);

      if (upw->higherWindow) {
         upw->higherWindow->lowerWindow = upw->lowerWindow;
      }
      if (upw->lowerWindow) {
         upw->lowerWindow->higherWindow = upw->higherWindow;
      }
      if (upw == up->topWindow) {
         up->topWindow = upw->lowerWindow;
      }

      free(upw);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindow_Lookup --
 *
 *      Retrieves the UnityPlatformWindow object associated with a given Window ID
 *
 * Results:
 *      The UnityPlatformWindow object, or NULL if it was not found
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

UnityPlatformWindow *
UPWindow_Lookup(UnityPlatform *up, // IN
                Window window)     // IN
{
   UnityPlatformWindow *retval = NULL;

   HashTable_Lookup(up->allWindows, GUINT_TO_POINTER(window), (void **)&retval);

   return retval;
}

#if 0 // Very useful if ever debugging the window stacking code, but slow otherwise.
/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowCheckStack --
 *
 *      Sanity check on the linked list of windows for Z-ordering.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May ASSERT() if things are broken.
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowCheckStack(UnityPlatform *up)
{
   UnityPlatformWindow **upwList;
   size_t numWindows;
   size_t i;
   UnityPlatformWindow *curWindow;

   HashTable_ToArray(up->allWindows,
                     (void ***)&upwList,
                     &numWindows);
   for (i = 0; i < numWindows; i++) {
      for (curWindow = up->topWindow;
           curWindow;
           curWindow = curWindow->lowerWindow) {
         if (curWindow == upwList[i]) {
            break;
         }
      }

      if (curWindow != upwList[i]) {
         Debug("%s: Wanted %p. Complete window stack is: ", __FUNCTION__, upwList[i]);
         for (curWindow = up->topWindow;
              curWindow;
              curWindow = curWindow->lowerWindow) {
            if (curWindow == upwList[i]) {
               Debug("%p ->", curWindow);
            } else {
               Debug("[%p] ->", curWindow);
            }
         }
         Debug("NULL\n");

         Debug("%s: Window stack downwards from %p: ", __FUNCTION__, upwList[i]);
         for (curWindow = upwList[i];
              curWindow;
              curWindow = curWindow->lowerWindow) {
            if (curWindow == upwList[i]) {
               Debug("[%p] ->", curWindow);
            } else {
               Debug("%p ->", curWindow);
            }
         }
         Debug("NULL\n");

         Debug("%s: Window stack upwards from %p: ", __FUNCTION__, upwList[i]);
         for (curWindow = upwList[i];
              curWindow;
              curWindow = curWindow->higherWindow) {
            if (curWindow == upwList[i]) {
               Debug("[%p] <-", curWindow);
            } else {
               Debug("%p <-", curWindow);
            }
         }
         Debug("NULL\n");
      }

      ASSERT(curWindow == upwList[i]);
   }
   for (curWindow = up->topWindow;
        curWindow;
        curWindow = curWindow->lowerWindow) {
      for (i = 0; i < numWindows; i++) {
         if (curWindow == upwList[i]) {
            break;
         }
      }
      ASSERT(i < numWindows); 
   }

   free(upwList);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowCheckCycle --
 *
 *      Checks to make sure there are no loops in the window stack.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May ASSERT().
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowCheckCycle(UnityPlatform *up)
{
   UnityPlatformWindow *upw;
   UnityPlatformWindow *curWindow;

   for (upw = up->topWindow; upw; upw = upw->lowerWindow) {
      for (curWindow = upw->lowerWindow; curWindow; curWindow = curWindow->lowerWindow) {
         ASSERT(curWindow != upw);
      }
   }

   for (upw = up->topWindow; upw->lowerWindow; upw = upw->lowerWindow) {
      /* Find lowest window */
   }

   for (; upw; upw = upw->higherWindow) {
      for (curWindow = upw->higherWindow; curWindow; curWindow = curWindow->higherWindow) {
         ASSERT(curWindow != upw);
      }
   }
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindow_Restack --
 *
 *      Changes the Z order list of the specified window so it is
 *      immediately above another window.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates UnityWindowTracker with the latest ZOrder.
 *
 *-----------------------------------------------------------------------------
 */

void
UPWindow_Restack(UnityPlatform *up,        // IN
		 UnityPlatformWindow *upw, // IN
		 Window above)             // IN - None indicates stack at bottom
{
   UnityPlatformWindow *newLowerWindow = NULL;

   ASSERT(up);
   ASSERT(upw);

   if (above != None) {
      newLowerWindow = UPWindow_Lookup(up, above);

      if (!newLowerWindow) {
         if (upw != up->topWindow) {
            Debug("BUG - couldn't find the window to stack above, placing at top.\n");
            newLowerWindow = up->topWindow;
         } else {
            return;
         }
      }
   }
   ASSERT(newLowerWindow != upw);

   if (newLowerWindow != upw->lowerWindow) {
      /*
       * Stacking order has changed. Move this window to the right place in the stack.
       *
       * 1. Remove 'upw' from the old location in the linked list.
       * 2. Find the 'upw' that it is now above.
       * 3. Insert it into the right location in the list.
       */

      ASSERT(upw->higherWindow != upw);
      ASSERT(upw->lowerWindow != upw);
      if (upw->higherWindow) {
         upw->higherWindow->lowerWindow = upw->lowerWindow;
      } else {
         up->topWindow = upw->lowerWindow;
      }

      ASSERT(upw->higherWindow != upw);
      ASSERT(upw->lowerWindow != upw);
      if (upw->lowerWindow) {
         upw->lowerWindow->higherWindow = upw->higherWindow;
      }
      upw->higherWindow = NULL;
      upw->lowerWindow = NULL;

      ASSERT(upw->higherWindow != upw);
      ASSERT(upw->lowerWindow != upw);
      upw->lowerWindow = newLowerWindow;
      if (newLowerWindow) {
         upw->higherWindow = newLowerWindow->higherWindow;
         upw->lowerWindow->higherWindow = upw;
         ASSERT(newLowerWindow != upw);
      } else {
         /*
          * This window is meant to go to the bottom of the stack.
          */
         upw->lowerWindow = NULL;
         upw->higherWindow = up->topWindow;

         while (upw->higherWindow && upw->higherWindow->lowerWindow) {
            upw->higherWindow = upw->higherWindow->lowerWindow;
         }
         ASSERT(newLowerWindow != upw);
      }

      ASSERT(newLowerWindow != upw);
      ASSERT(upw->higherWindow != upw);
      ASSERT(upw->lowerWindow != upw);
      if (upw->higherWindow) {
         ASSERT(upw->higherWindow->lowerWindow == newLowerWindow);
         upw->higherWindow->lowerWindow = upw;
      } else {
         up->topWindow = upw;
      }

      ASSERT(upw->higherWindow != upw);
      ASSERT(upw->lowerWindow != upw);
      if (upw->isRelevant) {
         up->stackingChanged = TRUE;
         Debug("Stacking order has changed\n");
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowSetRelevance --
 *
 *      Changes the "relevance" of a particular window. Normally, the decision of whether
 *      to do this should be made only by CheckRelevance().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May add or remove the window in UnityWindowTracker.
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowSetRelevance(UnityPlatform *up,        // IN
                     UnityPlatformWindow *upw, // IN
                     Bool isRelevant)          // IN
{
   if ((isRelevant && upw->isRelevant) || (!isRelevant && !upw->isRelevant)) {
      return;
   }

   upw->isRelevant = isRelevant;
   if (isRelevant) {
      Debug("Adding window %#lx to tracker\n", upw->toplevelWindow);
      UnityWindowTracker_AddWindowWithData(up->tracker, upw->toplevelWindow, upw);
      UPWindowPushFullUpdate(up, upw);
   } else {
      Debug("Removing window %#lx from tracker\n", upw->toplevelWindow);
      UnityWindowTracker_RemoveWindow(up->tracker, upw->toplevelWindow);
   }

   up->stackingChanged = TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindow_CheckRelevance --
 *
 *      Looks at the current state of a window to figure out whether we want to relay it
 *      through UnityWindowTracker.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates various cached metadata in UnityPlatformWindow, such as windowType and
 *      isOverrideRedirect.
 *
 *-----------------------------------------------------------------------------
 */

void
UPWindow_CheckRelevance(UnityPlatform *up,        // IN
                        UnityPlatformWindow *upw, // IN
                        const XEvent *motivator)  // IN - optional
{
   XWindowAttributes winAttr;
   int shouldBeRelevant = -1;
   Bool regetDesktop = FALSE;

   if (motivator) {
      /*
       * We have an event that may have modified the relevance of this window. See what
       * we need to do.
       */
      switch (motivator->type) {
      case PropertyNotify:
         if (motivator->xproperty.atom != up->atoms._NET_WM_WINDOW_TYPE
             && motivator->xproperty.atom != up->atoms._NET_WM_DESKTOP) {
            return;
         }
         if (motivator->xproperty.atom == up->atoms._NET_WM_DESKTOP) {
            regetDesktop = TRUE;
         }
         break;

      case ConfigureNotify:
         if ((motivator->xconfigure.override_redirect ? TRUE : FALSE)
             == upw->isOverrideRedirect) {
            return;
         }
         break;

      case UnmapNotify:
         /*
          * XXX should we ignore UnmapNotify events if they come from non-override
          * redirect windows?
          */

         /*
          * If a window is override redirect (e.g. tooltips), then we may need to show
          * & hide it based on map & unmap, because we won't get WM_STATE updates to
          * help us do minimize/restore.
          */
         break;

      case MapNotify:
         regetDesktop = TRUE;
         break;

      case ReparentNotify:
         {
            Window toplevelWindow;
            Window clientWindow;
            Window rootWindow;

            regetDesktop = TRUE;
            Debug("Window %#lx was reparented to a window %#lx\n",
                  motivator->xreparent.window, motivator->xreparent.parent);
            UnityPlatformFindWindows(up, motivator->xreparent.window,
                                     &toplevelWindow,
                                     &clientWindow,
                                     &rootWindow);
            UPWindowSetWindows(up, upw,
                               toplevelWindow,
                               clientWindow);
         }
         break;

      case DestroyNotify:
         shouldBeRelevant = FALSE;
         break;

      default:
         return;
      }
   } else {
      regetDesktop = TRUE;
   }

   if (shouldBeRelevant == -1) {
      Bool onCurrentDesktop = TRUE;
      Bool isInvisible = FALSE;
      Bool ignoreThisWindow = FALSE;

      UnityPlatformResetErrorCount(up);

      XGetWindowAttributes(up->display, upw->toplevelWindow, &winAttr);
      if (UnityPlatformGetErrorCount(up)) {
         shouldBeRelevant = FALSE;
         goto out;
      }

      if (regetDesktop) {
         if (!UPWindowGetDesktop(up, upw, &upw->desktopNumber)) {
            upw->desktopNumber = -1;
         }
      }
      if (upw->desktopNumber < up->desktopInfo.numDesktops
          && upw->desktopNumber >= 0
          && up->desktopInfo.guestDesktopToUnity[upw->desktopNumber] !=
          UnityWindowTracker_GetActiveDesktop(up->tracker)) {
         onCurrentDesktop = FALSE;
      }
      upw->isViewable = (winAttr.map_state == IsViewable);
      if (!upw->wasViewable) {
         if (upw->isViewable) {
            upw->wasViewable = upw->isViewable;
         } else {
            /*
             * Check if it's in iconic state (i.e. minimized), which means it was viewable
             * previously as far as we're concerned.
             */

            Atom propertyType;
            int propertyFormat;
            unsigned long itemsReturned = 0;
            unsigned long bytesRemaining;
            Atom *valueReturned = NULL;
            Window mainWindow = upw->clientWindow ? upw->clientWindow : upw->toplevelWindow;

            if (XGetWindowProperty(up->display, mainWindow, up->atoms.WM_STATE, 0,
                                   1024, False, AnyPropertyType,
                                   &propertyType, &propertyFormat, &itemsReturned,
                                   &bytesRemaining, (unsigned char **) &valueReturned)
                == Success
                && itemsReturned > 0
                && propertyType == up->atoms.WM_STATE
                && propertyFormat == 32
                && valueReturned[0] == IconicState) {
               upw->wasViewable = TRUE;
               Debug("Found window %#lx/%#lx initially in iconic state\n",
                     upw->toplevelWindow, upw->clientWindow);
            } else {
               upw->wasViewable = FALSE;
            }

            XFree(valueReturned);
         }
      }
      upw->isOverrideRedirect = winAttr.override_redirect ? TRUE : FALSE;

      if (winAttr.class == InputOnly) {
         isInvisible = TRUE;
      } else if (!upw->isViewable
                 && (!upw->wasViewable
                     || upw->isOverrideRedirect)
                 && onCurrentDesktop) {
         isInvisible = TRUE;
      } else if (winAttr.width <= 1 && winAttr.height <= 1) {
         isInvisible = TRUE;
      } else if ((winAttr.x + winAttr.width) < 0
                 || (winAttr.y + winAttr.height) < 0) {
         isInvisible = TRUE;
      }

      if (!isInvisible) {
         char *wmname = NULL;

         /*
          **************************************
          * This section should hold all the ugly app-specific filtering that might be
          * needed for UnityX11.
          */

         if (XFetchName(up->display,
                        upw->clientWindow ? upw->clientWindow : upw->toplevelWindow,
                        &wmname) != 0) {
            if (!strcmp(wmname, "gksu")
                && winAttr.override_redirect) {
               ignoreThisWindow = TRUE;
            }

            XFree(wmname);
         }

         /*
          * End app-specific filtering.
          *******************************************
          */
      }

      if (isInvisible) {
         shouldBeRelevant = FALSE;
      } else if (ignoreThisWindow) {
         shouldBeRelevant = FALSE;
      } else {
         Atom netWmWindowType = up->atoms._NET_WM_WINDOW_TYPE_NORMAL;
         Atom netWmPropertyType;
         int netWmPropertyFormat = 0;
         unsigned long itemsReturned, bytesRemaining;
         unsigned char *valueReturned = NULL;
         Window mainWindow;

         mainWindow = upw->clientWindow ? upw->clientWindow : upw->toplevelWindow;
         XGetWindowProperty(up->display, mainWindow,
                            up->atoms._NET_WM_WINDOW_TYPE, 0,
                            1024, False, AnyPropertyType,
                            &netWmPropertyType, &netWmPropertyFormat, &itemsReturned,
                            &bytesRemaining, &valueReturned);

         if (UnityPlatformGetErrorCount(up)) {
            Debug("Error retrieving window type property\n");
            shouldBeRelevant = FALSE;
            goto out;
         }

         if (netWmPropertyType == XA_ATOM && itemsReturned && !bytesRemaining) {
            netWmWindowType = *((Atom *) valueReturned);
         }
         XFree(valueReturned);

         shouldBeRelevant = TRUE;
         if (netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_DESKTOP) {
            shouldBeRelevant = FALSE;
            upw->windowType = UNITY_WINDOW_TYPE_DESKTOP;
            up->desktopWindow = upw;
         } else if (netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_DND) {
            shouldBeRelevant = FALSE;
         } else if (netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_DOCK) {
            shouldBeRelevant = up->currentSettings[UNITY_UI_TASKBAR_VISIBLE];
            upw->windowType = UNITY_WINDOW_TYPE_DOCK;
         } else if (netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_UTILITY) {
            upw->windowType = UNITY_WINDOW_TYPE_PANEL;
         } else if (netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_DIALOG) {
            upw->windowType = UNITY_WINDOW_TYPE_DIALOG;
         } else if (netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_MENU
                    || netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_POPUP_MENU
                    || netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_DROPDOWN_MENU) {
            upw->windowType = UNITY_WINDOW_TYPE_MENU;
         } else if (netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_SPLASH) {
            upw->windowType = UNITY_WINDOW_TYPE_SPLASH;
         } else if (netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_TOOLBAR) {
            upw->windowType = UNITY_WINDOW_TYPE_TOOLBAR;
         } else if (netWmWindowType == up->atoms._NET_WM_WINDOW_TYPE_TOOLTIP
                    || upw->isOverrideRedirect) {
            upw->windowType = UNITY_WINDOW_TYPE_TOOLTIP;
         } else {
            upw->windowType = UNITY_WINDOW_TYPE_NORMAL;
         }
      }
   }

  out:
   ASSERT(shouldBeRelevant >= 0);

   if (shouldBeRelevant) {
      Debug("Relevance for (%p) %#lx/%#lx/%#lx is %d (window type %d)\n",
            upw, upw->toplevelWindow, upw->clientWindow, upw->rootWindow,
            shouldBeRelevant, upw->windowType);
   }

   UPWindowSetRelevance(up, upw, shouldBeRelevant ? TRUE : FALSE);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindow_SetUserTime --
 *
 *      Updates the _NET_WM_USER_TIME property on a window so the window manager
 *      will let us restack the window.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updated timestamp.
 *
 *-----------------------------------------------------------------------------
 */

void
UPWindow_SetUserTime(UnityPlatform *up,        // IN
                     UnityPlatformWindow *upw) // IN
{
   Atom dummy;
   Window focusWindow;
   Atom propertyType;
   int propertyFormat;
   unsigned long itemsReturned, bytesRemaining;
   unsigned char *valueReturned = NULL;

   focusWindow = upw->clientWindow ? upw->clientWindow : upw->toplevelWindow;

   XGetWindowProperty(up->display, focusWindow, up->atoms._NET_WM_USER_TIME_WINDOW, 0,
                      1024, False, XA_WINDOW, &propertyType, &propertyFormat,
                      &itemsReturned, &bytesRemaining, &valueReturned);
   if (valueReturned) {
      focusWindow = *(Window *)valueReturned;
      XFree(valueReturned);
   }

   dummy = UnityPlatformGetServerTime(up);
   XChangeProperty(up->display, focusWindow,
                   up->atoms._NET_WM_USER_TIME, XA_CARDINAL,
                   32, PropModeReplace, (unsigned char *)&dummy, 1);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowGetActualWindowAndPosition --
 *
 *      Figures out the right arguments to call XMoveResizeWindow with when
 *      moving/resizing a window.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Puts the window and coordinates to use in 'actualWindow' and 'actualRect'.
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowGetActualWindowAndPosition(UnityPlatform *up,                // IN
                                   const UnityPlatformWindow *upw,   // IN
                                   const UnityRect *orig,            // IN
                                   const XWindowAttributes *origTop, // IN
                                   Window *actualWindow,             // IN/OUT
                                   UnityRect *actualRect)            // IN/OUT
{
   XWindowAttributes clientWinAttr;
   Atom propertyType;
   int propertyFormat = 0;
   unsigned long itemsReturned = 0;
   unsigned long bytesRemaining;
   unsigned char *valueReturned = NULL;
   int frameSizeTop;
   int frameSizeBottom;
   int frameSizeLeft;
   int frameSizeRight;

   ASSERT(up);
   ASSERT(upw);
   ASSERT(orig);
   ASSERT(actualWindow);
   ASSERT(actualRect);

   *actualRect = *orig;
   if (!upw->clientWindow) {
      *actualWindow = upw->toplevelWindow;
      return;
   }

   *actualWindow = upw->clientWindow;

   /*
    * We need to figure out how to adjust the 'orig' rect (which is in toplevelWindow
    * coordinates) and turn it into clientWindow coordinates. Because window managers
    * ignore requests to modify their frame windows (toplevelWindow), we have to request
    * the change on the clientWindow instead.
    */
   if (UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_FRAME_EXTENTS)
       && XGetWindowProperty(up->display, upw->clientWindow, up->atoms._NET_FRAME_EXTENTS, 0,
                             1024, False, XA_CARDINAL,
                             &propertyType, &propertyFormat, &itemsReturned,
                             &bytesRemaining, &valueReturned) == Success
       && propertyFormat == 32
       && itemsReturned >= 4) {
      Atom *atomValue = (Atom *)valueReturned;

      frameSizeLeft = atomValue[0];
      frameSizeRight = atomValue[1];
      frameSizeTop = atomValue[2];
      frameSizeBottom = atomValue[3];
   } else {
      /*
       * Query the current clientWindow and calculate how to adjust the frame coords to
       * client coords.
       */
      clientWinAttr.x = clientWinAttr.y = 0;
      clientWinAttr.width = origTop->width;
      clientWinAttr.height = origTop->height;

      XGetWindowAttributes(up->display, upw->clientWindow, &clientWinAttr);

      frameSizeLeft = clientWinAttr.x;
      frameSizeRight = origTop->width - (clientWinAttr.x + clientWinAttr.width);
      frameSizeTop = clientWinAttr.y;
      frameSizeBottom = origTop->height - (clientWinAttr.y + clientWinAttr.height);
   }

   /*
    * It turns out that with metacity, we don't have to adjust x/y for the frame size,
    * only the width & height. XXX we should see how other window managers behave.
    */
#if 0
   actualRect->x += frameSizeLeft;
   actualRect->y += frameSizeTop;
#endif

   actualRect->width -= frameSizeLeft + frameSizeRight;
   actualRect->height -= frameSizeTop + frameSizeBottom;

   XFree(valueReturned);
}


/*
 *----------------------------------------------------------------------------
 *
 * UnityPlatformMoveResizeWindow --
 *
 *      Moves and/or resizes the given window to the specified location. Does not
 *      attempt to move and/or resize window if (a) the destination rectangle does not
 *      intersect with the virtual screen rectangle (b) window is minimized.
 *
 *      If the input width & height match the current width & height, then this
 *      function will end up just moving the window. Similarly if the input
 *      x & y coordinates match the current coordinates, then it will end up just
 *      resizing the window.
 *
 * Results:
 *      Even if the move/resize operaion is not execuated or it fails, window's
 *      current coordinates are always sent back.
 *
 *      Function does not return FALSE if the attempt to move and/or resize fails.
 *      This is because the host will be comparing input and output parameters to
 *      decide whether the window really moved and/or resized.
 *
 *      In a very rare case, when attempt to get window's current coordinates fail,
 *      returns FALSE.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

Bool
UnityPlatformMoveResizeWindow(UnityPlatform *up,         // IN
                              UnityWindowId window,      // IN: Window handle
                              UnityRect *moveResizeRect) // IN/OUT: Desired coordinates,
                                                         // before and after
                                                         // the operation.
{
   UnityPlatformWindow *upw;
   Bool retval = FALSE;
   XWindowAttributes winAttr;
   UnityRect desiredRect;

   ASSERT(moveResizeRect);

   upw = UPWindow_Lookup(up, window);
   if (!upw) {
      return FALSE;
   }

   desiredRect = *moveResizeRect;

   if (upw->lastConfigureEvent) {
      free(upw->lastConfigureEvent);
      upw->lastConfigureEvent = NULL;
   }

   UnityPlatformResetErrorCount(up);
   XGetWindowAttributes(up->display, upw->toplevelWindow, &winAttr);
   if (UnityPlatformGetErrorCount(up)) {
      return FALSE;
   }

   if (winAttr.x == moveResizeRect->x
       && winAttr.y == moveResizeRect->y
       && winAttr.width == moveResizeRect->width
       && winAttr.height == moveResizeRect->height) {
      return TRUE;
   }

   /*
    * _NET_MOVERESIZE_WINDOW is preferable in general (because it saves us extra X
    * calls), but it is broken in metacity and there's no way to detect whether it works
    * or not.
    */
#if defined(VM_CAN_TRUST__NET_MOVERESIZE_WINDOW)
   if (UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_MOVERESIZE_WINDOW)
       && upw->clientWindow) {
      Atom data[5] = {0, 0, 0, 0, 0};

      /*
       * The first datum in the EWMH _NET_MOVERESIZE_WINDOW message contains a bunch of
       * bits to signify what information is in the request and where the request came
       * from. The (0xF << 8) bits (lower nibble of the upper byte) signify that the request contains x, y, width, and
       * height data. The (2 << 12) bits (higher nibble of the upper byte) signifies that the request was made by the
       * pager/taskbar. StaticGravity is 10 in the lower byte.
       */
      data[0] = (0xF << 8) | (2 << 12) | StaticGravity;

      data[1] = moveResizeRect->x;
      data[2] = moveResizeRect->y;
      data[3] = moveResizeRect->width;
      data[4] = moveResizeRect->height;
      UnityPlatformSendClientMessage(up, upw->rootWindow, upw->clientWindow,
                                     up->atoms._NET_MOVERESIZE_WINDOW,
                                     32, 5, data);
      Debug("MoveResizeWindow implemented using _NET_MOVERESIZE_WINDOW\n");
   } else
#endif
   {
      UnityRect actualRect;
      Window actualWindow;

      UPWindowGetActualWindowAndPosition(up, upw, moveResizeRect, &winAttr, &actualWindow, &actualRect);

      XMoveResizeWindow(up->display, actualWindow,
                        actualRect.x, actualRect.y,
                        actualRect.width, actualRect.height);
      Debug("MoveResizeWindow implemented using XMoveResizeWindow (requested (%d, %d) +(%d, %d) on %#lx\n",
            actualRect.x, actualRect.y, actualRect.width, actualRect.height,
            actualWindow);
   }

   /*
    * Protect against the window being destroyed while we're waiting for results of the
    * resize.
    */
   UPWindow_Ref(up, upw);

   /*
    * Because the window manager may take a non-trivial amount of time to process the
    * move/resize request, we have to spin here until a ConfigureNotify event is
    * generated on the window.
    */
   while (!upw->lastConfigureEvent) {
      Debug("Running main loop iteration\n");
      UnityPlatformProcessMainLoop(); // Process events, do other Unity stuff, etc.
   }

   if (upw->lastConfigureEvent && upw->lastConfigureEvent->window == upw->toplevelWindow) {
      moveResizeRect->x = upw->lastConfigureEvent->x;
      moveResizeRect->y = upw->lastConfigureEvent->y;
      moveResizeRect->width = upw->lastConfigureEvent->width;
      moveResizeRect->height = upw->lastConfigureEvent->height;

      retval = TRUE;
   } else {
      /*
       * There are cases where we only get a ConfigureNotify on the clientWindow because
       * no actual change happened, in which case we just verify that we have the right
       * toplevelWindow position and size.
       */
      Debug("Didn't get lastConfigureEvent on the toplevel window - requerying\n");

      XGetWindowAttributes(up->display, upw->toplevelWindow, &winAttr);
      moveResizeRect->x = winAttr.x;
      moveResizeRect->y = winAttr.y;
      moveResizeRect->width = winAttr.width;
      moveResizeRect->height = winAttr.height;

      retval = TRUE;
   }

   Debug("MoveResizeWindow(%#lx/%#lx): original (%d,%d)+(%d,%d), desired (%d,%d)+(%d,%d), actual (%d,%d)+(%d,%d) = %d\n",
         upw->toplevelWindow, upw->clientWindow,
         winAttr.x, winAttr.y, winAttr.width, winAttr.height,
         desiredRect.x, desiredRect.y, desiredRect.width, desiredRect.height,
         moveResizeRect->x, moveResizeRect->y,
         moveResizeRect->width, moveResizeRect->height,
         retval);

   UPWindow_Unref(up, upw);

   return retval;
}


/*
 *----------------------------------------------------------------------------
 *
 * UnityPlatformCloseWindow --
 *
 *      Close the window. Send WM_DELETE message to the specified window.
 *      Just because the message was posted successfully, doesn't mean the
 *      window will be closed (this is up to the underlying application/user
 *      actions).
 *
 * Results:
 *      TRUE if we were successful in posting the close message to the window.
 *      FALSE otherwise.
 *
 * Side effects:
 *      The window might be closed (and, potentially, an application exits)
 *
 *----------------------------------------------------------------------------
 */

Bool
UnityPlatformCloseWindow(UnityPlatform *up,         // IN: Platform data
                         UnityWindowId window)      // IN: window handle
{
   UnityPlatformWindow *upw;

   ASSERT(up);

   upw = UPWindow_Lookup(up, window);

   Debug("Closing window %#x\n", window);

   if (!upw) {
      return FALSE;
   }

   if (upw->clientWindow &&
       UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_CLOSE_WINDOW)) {
      Atom data[] = {0,
                     2, // Bit to indicate that the pager requested it
                     0,
                     0,
                     0};
      data[0] = UnityPlatformGetServerTime(up);
      UnityPlatformSendClientMessage(up, upw->rootWindow, upw->clientWindow,
                                     up->atoms._NET_CLOSE_WINDOW, 32, 5,
                                     data);
   } else if (UPWindow_ProtocolSupported(up, upw, UNITY_X11_WIN_WM_DELETE_WINDOW)){
      Atom data[1];
      Window destWin = upw->clientWindow ? upw->clientWindow : upw->toplevelWindow;

      data[0] = up->atoms.WM_DELETE_WINDOW;

      UnityPlatformSendClientMessage(up, destWin, destWin,
                                     up->atoms.WM_DELETE_WINDOW, 32, 1,
                                     data);
   } else {
      XDestroyWindow(up->display, upw->toplevelWindow);
      XFlush(up->display);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UnityPlatformArgvToWindowPath --
 *
 *      Encodes the string array 'argv' into the DynBuf 'buf'.
 *
 * Results:
 *      TRUE if successful, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static char *
UnityPlatformArgvToWindowPath(UnityPlatform *up,        // IN
                              UnityPlatformWindow *upw, // IN
                              char **inArgv,            // IN
                              int argc,                 // IN
                              char *cwd)                // IN
{
   int i;
   int err;
   char *ctmp = NULL;
   char **argv;
   char *queryString = NULL;
   char *uriString = NULL;

   ASSERT(argc);

   argv = inArgv;

#ifdef GTK2
   while (argc && AppUtil_AppIsSkippable(argv[0])) {
      argv++;
      argc--;
   }

   if (!argc) {
      return NULL;
   }

   if (argv[0][0] != '/') {
      if ((ctmp = AppUtil_CanonicalizeAppName(argv[0], cwd))) {
         char **newArgv;
         newArgv = alloca(argc * sizeof argv[0]);
         memcpy(newArgv, argv, argc * sizeof argv[0]);
         argv = newArgv;
         i = strlen(ctmp) + 1;
         argv[0] = alloca(i);
         memcpy(argv[0], ctmp, i);
         g_free(ctmp);
      } else {
         Debug("%s: Program %s not found\n", __FUNCTION__, argv[0]);
         return NULL;
      }
   }

#endif

   if (argc > 1 || upw) {
      UriQueryListA *queryList;
      char windowIdBuf[32];
      int j;

      queryList = alloca((argc + 1) * sizeof *queryList);
      for (i = 1, j = 0; i < argc; i++, j++) {
         queryList[j].key = "argv[]";
         queryList[j].value = argv[i];
         queryList[j].next = &queryList[j + 1];
      }
      if (upw) {
         /*
          * The XID is used to allow GHI to retrieve icons for more apps...
          */
         queryList[j].key = "WindowXID";
         Str_Sprintf(windowIdBuf, sizeof windowIdBuf, "%lu",
                     upw->clientWindow ? upw->clientWindow : upw->toplevelWindow);
         queryList[j].value = windowIdBuf;
         queryList[j].next = &queryList[j + 1];
         j++;
      }
      queryList[j - 1].next = NULL;

      if (uriComposeQueryMallocA(&queryString, queryList)) {
         Debug("uriComposeQueryMallocA failed\n");
         return NULL;
      }
   }

   uriString = alloca(10 + 3 * strlen(argv[0])); // This formula comes from URI.h
   err = uriUnixFilenameToUriStringA(argv[0], uriString);
   if (err) {
      Debug("uriUnixFilenameToUriStringA failed\n");
      return NULL;
   }

   if (queryString) {
      /*
       * We could use uriparser to construct the whole URI with querystring for us, but
       * there doesn't seem to be any advantage to that right now, and it'd involve more
       * steps.
       */
      return g_strdup_printf("%s?%s", uriString, queryString);
   } else {
      return g_strdup(uriString);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UnityPlatformReadProcessPath --
 *
 *      Reads the cmdline of a process and stuffs it into the supplied DynBuf in
 *      URI-encoded form.
 *
 * Results:
 *      TRUE if successful, FALSE otherwise.
 *
 * Side effects:
 *      Stores a NULL-terminated string in 'buf' for later use by GHI.
 *
 *-----------------------------------------------------------------------------
 */

static char *
UnityPlatformReadProcessPath(UnityPlatform *up,        // IN
                             UnityPlatformWindow *upw, // IN
                             pid_t pid)                // IN
{
#if defined(linux)
   FILE *fh;
   char cbuf[256];
   char cwdbuf[PATH_MAX];
   int i;

   Str_Snprintf(cbuf, sizeof cbuf, "/proc/%d/cwd", pid);
   i = readlink(cbuf, cwdbuf, sizeof cwdbuf);
   if (i <= 0) {
      return NULL;
   }
   cwdbuf[i] = '\0';

   Str_Snprintf(cbuf, sizeof cbuf, "/proc/%d/cmdline", pid);

   fh = fopen(cbuf, "r");
   if (fh) {
      size_t nitems;
      char *argv[2048];
      int argc, i;

      nitems = fread(cbuf, 1, sizeof cbuf, fh);
      fclose(fh);

      if (!nitems) {
         return FALSE;
      }

      for (argc = i = 0; i < nitems; i++) {
         if (i == 0 || cbuf[i - 1] == '\0') {
            argv[argc++] = &cbuf[i];
         }
      }
      argv[argc] = NULL;

      return UnityPlatformArgvToWindowPath(up, upw, argv, argc, cwdbuf);
   }
#endif

   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UnityX11GetWindowPath --
 *
 *      Internal routine used to retrieve the window path for the purpose of getting its
 *      icons and for the unity.get.window.path operation.
 *
 * Results:
 *      Newly allocated window path if successful, NULL otherwise.
 *
 * Side effects:
 *      Allocates memory to be returned to caller.
 *
 *-----------------------------------------------------------------------------
 */

static char *
UnityX11GetWindowPath(UnityPlatform *up,
                      UnityPlatformWindow *upw)
{

   Atom propertyType;
   int propertyFormat;
   unsigned long itemsReturned = 0;
   unsigned long bytesRemaining;
   unsigned char *valueReturned = NULL;
   char **argv = NULL;
   int argc;
   XClassHint classHint = {NULL, NULL};
   int ret;
   Window checkWindow;
   char *retval = NULL;

   checkWindow = upw->clientWindow ? upw->clientWindow : upw->toplevelWindow;

   UnityPlatformResetErrorCount(up);
   ret = XGetWindowProperty(up->display, checkWindow, up->atoms._NET_WM_PID, 0,
                            1024, False, AnyPropertyType,
                            &propertyType, &propertyFormat, &itemsReturned,
                            &bytesRemaining, &valueReturned);
   if (UnityPlatformGetErrorCount(up) || ret != Success) {
      return NULL;
   }

   if (propertyType == XA_CARDINAL && itemsReturned >= 1) {
      pid_t windowPid = 0;

      switch (propertyFormat) {
      case 16:
         windowPid = * (CARD16 *)valueReturned;
         break;
      case 32:
         windowPid = *(XID *)valueReturned;
         break;
      default:
         Debug("Unknown propertyFormat %d while retrieving _NET_WM_PID\n",
               propertyFormat);
         break;
      }

      if (windowPid) {
         retval = UnityPlatformReadProcessPath(up, upw, windowPid);
      }
   }
   XFree(valueReturned);

   if (!retval && XGetCommand(up->display, checkWindow, &argv, &argc)) {
      retval = UnityPlatformArgvToWindowPath(up, upw, argv, argc, NULL);

      XFreeStringList(argv);
   }

   if (!retval && XGetClassHint(up->display, checkWindow, &classHint)) {
      /*
       * Last-ditch - try finding the WM_CLASS on $PATH.
       */

      char *fakeArgv[2] = {NULL, NULL};

      if (classHint.res_name && *classHint.res_name) {
         fakeArgv[0] = classHint.res_name;
      } else if (classHint.res_class && *classHint.res_class) {
         fakeArgv[0] = classHint.res_class;
      }

      if (fakeArgv[0] && *(fakeArgv[0])) {
         retval = UnityPlatformArgvToWindowPath(up, upw, fakeArgv, 1, NULL);
      }

      XFree(classHint.res_name);
      XFree(classHint.res_class);
   }

   Debug("UnityX11GetWindowPath(%#lx) returning \"%s\"\n",
         upw->toplevelWindow, retval);

   return retval;
}


/*
 *----------------------------------------------------------------------------
 *
 * UnityPlatformGetWindowPath --
 *
 *      Get the information needed to re-launch a window and retrieve further information
 *      on it. 'buf' should hold a null-terminated string that will be passed to the
 *      UnityPlatformGetBinaryInfo() and GHIntegrationShellOpen() routines. The exact
 *      meaning of its contents is platform-specific.
 *
 * Results:
 *      TRUE if everything went ok, FALSE otherwise.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------------
 */

Bool
UnityPlatformGetWindowPath(UnityPlatform *up,        // IN: Platform data
                           UnityWindowId window,     // IN: window handle
                           DynBuf *buf)              // IN/OUT: full path to the binary
{
   UnityPlatformWindow *upw;
   Bool retval = FALSE;
   char *retstr;

   ASSERT(up);

   upw = UPWindow_Lookup(up, window);

   if (!upw) {
      Debug("GetWindowPath FAILED!\n");
      return FALSE;
   }

   retstr = UnityX11GetWindowPath(up, upw);

   if (!retstr) {
      Debug("GetWindowPath didn't know how to identify the window...\n");
   } else {
      Debug("GetWindowPath window %#x results in: %s\n",
            window, retstr);
      DynBuf_AppendString(buf, retstr);
      g_free(retstr);
      retval = TRUE;
   }

   return retval;
}


/*
 *------------------------------------------------------------------------------
 *
 * UnityPlatformGetWindowContents --
 *
 *     Read the correct bits off the window regardless of whether it's minimized
 *     or obscured. Return the result as a PNG in the imageData DynBuf.
 *
 * Results:
 *     Returns TRUE in case success and FALSE otherwise.
 *
 * Side effects:
 *     None.
 *
 *------------------------------------------------------------------------------
 */

Bool
UnityPlatformGetWindowContents(UnityPlatform *up,     // IN
                               UnityWindowId window,  // IN
                               DynBuf *imageData)     // IN
{
   UnityPlatformWindow *upw;
   Pixmap pixmap = 0;
   XWindowAttributes attrs;
   XImage *ximage = NULL;
   GC xgc = 0;
   XGCValues gcvalues;
   Bool result = FALSE;
   ImageInfo vmimage = { 0 };

   ASSERT(up);
   ASSERT(imageData);

   upw = UPWindow_Lookup(up, window);

   if (!upw) {
      return FALSE;
   }

   UnityPlatformResetErrorCount(up);
   if (!XGetWindowAttributes(up->display, upw->toplevelWindow, &attrs)
       || UnityPlatformGetErrorCount(up)) {
      return FALSE;
   }
   pixmap = XCreatePixmap(up->display, upw->toplevelWindow,
                          attrs.width, attrs.height, attrs.depth);
   if (UnityPlatformGetErrorCount(up)) {
      return FALSE;
   }

   gcvalues.background = gcvalues.foreground = 0;
   gcvalues.subwindow_mode = IncludeInferiors;
   gcvalues.fill_style = FillSolid;
   XFillRectangle(up->display, pixmap, xgc, 0, 0, attrs.width, attrs.height);
   xgc = XCreateGC(up->display, pixmap,
                   GCFillStyle |
                   GCBackground |
                   GCForeground |
                   GCSubwindowMode,
                   &gcvalues);
   if (UnityPlatformGetErrorCount(up)) {
      goto out;
   }

   XCopyArea(up->display, upw->toplevelWindow, pixmap, xgc, 0, 0,
             attrs.width, attrs.height, 0, 0);
   if (UnityPlatformGetErrorCount(up)) {
      goto out;
   }

   ximage = XGetImage(up->display, pixmap, 0, 0,
                      attrs.width, attrs.height, ~0, XYPixmap);

   if (!ximage || UnityPlatformGetErrorCount(up)) {
      goto out;
   }

   vmimage.width = ximage->width;
   vmimage.height = ximage->height;
   vmimage.depth = ximage->depth;
   vmimage.bpp = ximage->bitmap_unit;
   vmimage.redMask = ximage->red_mask;
   vmimage.greenMask = ximage->green_mask;
   vmimage.blueMask = ximage->blue_mask;
   vmimage.bytesPerLine = ximage->bytes_per_line;
   vmimage.data = ximage->data;

   if (ImageUtil_ConstructPNGBuffer(&vmimage, imageData)) {
      result = TRUE;
   }

  out:
   if (ximage) {
      XDestroyImage(ximage);
   }

   if (xgc) {
      XFreeGC(up->display, xgc);
   }

   if (pixmap) {
      XFreePixmap(up->display, pixmap);
   }

   return result;
}


/*
 *------------------------------------------------------------------------------
 *
 * UnityPlatformGetIconData --
 *
 *     Read part or all of a particular icon on a window.  Return the result as a PNG in
 *     the imageData DynBuf, and also return the full length of the PNG in fullLength.
 *
 * Results:
 *     Returns TRUE if successful, and FALSE otherwise.
 *
 * Side effects:
 *     None.
 *
 *------------------------------------------------------------------------------
 */

Bool
UnityPlatformGetIconData(UnityPlatform *up,       // IN
                         UnityWindowId window,    // IN
                         UnityIconType iconType,  // IN
                         UnityIconSize iconSize,  // IN
                         uint32 dataOffset,       // IN
                         uint32 dataLength,       // IN
                         DynBuf *imageData,       // OUT
                         uint32 *fullLength)      // OUT
{
   UnityPlatformWindow *upw;

   ASSERT(up);
   ASSERT(fullLength);
   ASSERT(imageData);

   upw = UPWindow_Lookup(up, window);

   if (!upw || !upw->clientWindow || iconType != UNITY_ICON_TYPE_MAIN) {
      return FALSE;
   }

   Debug("GetIconData %#lx\n", (Window)window);

   if (!DynBuf_GetSize(&upw->iconPng.data)
       || (upw->iconPng.size != iconSize)
       || (upw->iconPng.type != iconType)) {
      GPtrArray *pixbufs;
      Bool gotIcons = FALSE;
      char *windowPath;

      windowPath = UnityX11GetWindowPath(up, upw);
      pixbufs = AppUtil_CollectIconArray(windowPath, upw->clientWindow);
      g_free(windowPath);

      if (pixbufs && pixbufs->len) {
         GdkPixbuf *pixbuf;
         gchar *pngData;
         gsize pngDataSize;

         pixbuf = g_ptr_array_index(pixbufs, 0);

         if (gdk_pixbuf_save_to_buffer(pixbuf, &pngData, &pngDataSize,
                                       "png", NULL, NULL)) {
            DynBuf_Attach(&upw->iconPng.data, pngDataSize, pngData);
            gotIcons = TRUE;
         } else {
            DynBuf_SetSize(&upw->iconPng.data, 0);
         }

         upw->iconPng.size = iconSize;
         upw->iconPng.type = iconType;
      }

      AppUtil_FreeIconArray(pixbufs);

      if (!gotIcons) {
         return FALSE;
      }
   }

   *fullLength = DynBuf_GetSize(&upw->iconPng.data);
   if (dataOffset >= *fullLength) {
      DynBuf_SetSize(imageData, 0);
   } else {
      uint32 realLength;

      if ((dataOffset + dataLength) > *fullLength) {
         realLength = *fullLength - dataOffset;
      } else {
         realLength = dataLength;
      }
      DynBuf_Enlarge(imageData, realLength);
      DynBuf_SetSize(imageData, realLength);

      memcpy(imageData->data,
             ((const char *)DynBuf_Get(&upw->iconPng.data)) + dataOffset,
             realLength);
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * UnityPlatformRestoreWindow --
 *
 *      Tell the window to restore from the minimized state to its original
 *      size.
 *
 * Results:
 *      TRUE always
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
UnityPlatformRestoreWindow(UnityPlatform *up,    // IN
                           UnityWindowId window) // IN
{
   UnityPlatformWindow *upw;

   ASSERT(up);

   upw = UPWindow_Lookup(up, window);

   if (!upw || !upw->clientWindow) {
      Debug("Restoring FAILED!\n");
      return FALSE;
   }

   if (upw->isMinimized) {
      Atom data[5] = {0, 0, 0, 0, 0};

      Debug("Restoring window %#x\n", window);

      upw->isMinimized = FALSE;
      upw->wantInputFocus = TRUE;

      /*
       * Unfortunately the _NET_WM_STATE messages only work for windows that are already
       * mapped, i.e. not iconified or withdrawn.
       */
      if (!upw->isHidden) {
         XMapRaised(up->display, upw->clientWindow);
      }

      data[0] = _NET_WM_STATE_REMOVE;
      data[1] = up->atoms._NET_WM_STATE_MINIMIZED;
      data[2] = up->atoms._NET_WM_STATE_HIDDEN;
      data[3] = 2; // Message is from the pager/taskbar
      UnityPlatformSendClientMessage(up, upw->rootWindow, upw->clientWindow,
                                     up->atoms._NET_WM_STATE, 32, 4, data);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowProcessPropertyEvent --
 *
 *      Processes an notification that a property has changed on an X11 window.
 *
 *      N.B. The UPWindowPushFullUpdate method creates a fake 'xevent' based on all the
 *      properties are set initially on a window, so before passing 'xevent' down the
 *      call chain, or using any additional fields from it, make sure that
 *      UPWindowPushFullUpdate fills in those fields properly.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowProcessPropertyEvent(UnityPlatform *up,        // IN
                             UnityPlatformWindow *upw, // IN
                             const XEvent *xevent)     // IN
{
   Atom eventAtom;

   ASSERT(up);
   ASSERT(upw);
   ASSERT(xevent);

   eventAtom = xevent->xproperty.atom;
   if (eventAtom == up->atoms._NET_WM_STATE
       || eventAtom == up->atoms.WM_STATE) {
      UPWindowUpdateState(up, upw);
      if (eventAtom == up->atoms.WM_STATE) {
         UPWindowUpdateIcon(up, upw);
      }
   } else if (eventAtom == up->atoms.WM_NAME) {
      UPWindowUpdateTitle(up, upw);
   } else if (eventAtom == up->atoms.WM_PROTOCOLS) {
      UPWindowUpdateProtocols(up, upw);
   } else if (eventAtom == up->atoms._NET_WM_ALLOWED_ACTIONS) {
      UPWindowUpdateActions(up, upw);
   } else if (eventAtom == up->atoms._NET_WM_WINDOW_TYPE) {
      UPWindowUpdateType(up, upw);
   } else if (eventAtom == up->atoms._NET_WM_ICON
              || eventAtom == up->atoms.WM_ICON) {
      UPWindowUpdateIcon(up, upw);
   } else if (eventAtom == up->atoms._NET_WM_DESKTOP) {
      UPWindowUpdateDesktop(up, upw);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowProcessConfigureEvent --
 *
 *      Processes an notification that the window configuration has changed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowProcessConfigureEvent(UnityPlatform *up,        // IN
                              UnityPlatformWindow *upw, // IN
                              const XEvent *xevent)     // IN
{
   if (xevent->xconfigure.window == upw->toplevelWindow) {
      const int border_width = xevent->xconfigure.border_width;
      const int x = xevent->xconfigure.x;
      const int y = xevent->xconfigure.y;

      /*
       * Used for implementing the move_resize operation.
       */
      if (!upw->lastConfigureEvent) {
         upw->lastConfigureEvent = Util_SafeMalloc(sizeof *upw->lastConfigureEvent);
      }
      *upw->lastConfigureEvent = xevent->xconfigure;

      Debug("Moving window %#lx/%#lx to (%d, %d) +(%d, %d)\n",
            upw->toplevelWindow, upw->clientWindow,
            x - border_width,
            y - border_width,
            xevent->xconfigure.width + border_width,
            xevent->xconfigure.height + border_width);

      UnityWindowTracker_MoveWindow(up->tracker, upw->toplevelWindow,
                                    x - border_width,
                                    y - border_width,
                                    x + xevent->xconfigure.width + border_width,
                                    y + xevent->xconfigure.height + border_width);

      if ((xevent->xconfigure.above != None && !upw->lowerWindow)
	  || (xevent->xconfigure.above == None && upw->lowerWindow)
	  || (upw->lowerWindow && xevent->xconfigure.above
              != upw->lowerWindow->toplevelWindow)) {
         Debug("Marking window %#lx/%#lx for restacking\n",
               upw->toplevelWindow, upw->clientWindow);
         UPWindow_Restack(up, upw, xevent->xconfigure.above);
      }
   } else {
      if (!upw->lastConfigureEvent) {
         upw->lastConfigureEvent = Util_SafeMalloc(sizeof *upw->lastConfigureEvent);
         *upw->lastConfigureEvent = xevent->xconfigure;
      }
      Debug("ProcessConfigureEvent skipped event on window %#lx (upw was %#lx/%#lx)\n",
            xevent->xconfigure.window, upw->toplevelWindow, upw->clientWindow);
   }
}


#if defined(VM_HAVE_X11_SHAPE_EXT)


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowUpdateShape --
 *
 *      Updates the window shape.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Notification to the window tracker
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowUpdateShape(UnityPlatform *up,        // IN
                    UnityPlatformWindow *upw) // IN
{
   RegionPtr clipRegion = NULL;
   RegionPtr boundingRegion = NULL;
   RegionPtr region = NULL;
   XRectangle *rects = NULL;
   int rectCount;
   int rectOrdering;

   /*
    * Retrieve the X11 'clipping shape' (the window shape including its border) and turn
    * it into a region.
    */
   UnityPlatformResetErrorCount(up);
   rects = XShapeGetRectangles(up->display, upw->toplevelWindow, ShapeClip,
                               &rectCount, &rectOrdering);
   if (!UnityPlatformGetErrorCount(up) && rects && rectCount) {
      xRectangle *vmRects;
      int i;

      vmRects = alloca(rectCount * (sizeof *vmRects));
      memset(vmRects, 0, rectCount * (sizeof *vmRects));
      for (i = 0; i < rectCount; i++) {
         ASSERT(rects[i].width);
         ASSERT(rects[i].height);
         vmRects[i].x = rects[i].x;
         vmRects[i].y = rects[i].y;
         vmRects[i].width = rects[i].width;
         vmRects[i].height = rects[i].height;
         vmRects[i].info.type = UpdateRect;
      }

      clipRegion = miRectsToRegion(rectCount, vmRects, 0);
   }
   XFree(rects); rects = NULL;
   rectCount = 0;

   UnityPlatformResetErrorCount(up);

   /*
    * Retrieve the X11 'clipping shape' (the window shape of the window without its
    * border) and turn it into a region.
    */
   rects = XShapeGetRectangles(up->display, upw->toplevelWindow, ShapeBounding,
                               &rectCount, &rectOrdering);
   if (!UnityPlatformGetErrorCount(up)
       && rects && rectCount) {
      xRectangle *vmRects;
      int i;

      vmRects = alloca(rectCount * (sizeof *vmRects));
      memset(vmRects, 0, rectCount * (sizeof *vmRects));
      for (i = 0; i < rectCount; i++) {
         ASSERT(rects[i].width);
         ASSERT(rects[i].height);
         vmRects[i].x = rects[i].x;
         vmRects[i].y = rects[i].y;
         vmRects[i].width = rects[i].width;
         vmRects[i].height = rects[i].height;
         vmRects[i].info.type = UpdateRect;
      }

      boundingRegion = miRectsToRegion(rectCount, vmRects, 0);
   }
   XFree(rects);

   if (boundingRegion && clipRegion) {
      region = miRegionCreate(NULL, 2);
      miIntersect(region, clipRegion, boundingRegion);
   } else if (clipRegion) {
      region = clipRegion; clipRegion = NULL;
   } else if (boundingRegion) {
      region = boundingRegion; boundingRegion = NULL;
   }

   UnityWindowTracker_ChangeWindowRegion(up->tracker, upw->toplevelWindow, region);
   if (clipRegion) {
      miRegionDestroy(clipRegion);
   }
   if (boundingRegion) {
      miRegionDestroy(boundingRegion);
   }
   if (region) {
      miRegionDestroy(region);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowProcessShapeEvent --
 *
 *      Processes an notification that the non-rectangular window shape has changed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowProcessShapeEvent(UnityPlatform *up,        // IN
                          UnityPlatformWindow *upw, // IN
                          const XEvent *xevent)     // IN
{
   XShapeEvent *sev;

   ASSERT(up);
   ASSERT(upw);
   ASSERT(xevent->type == (up->shapeEventBase + ShapeNotify));

   sev = (XShapeEvent *)xevent;
   ASSERT (sev->window == upw->toplevelWindow);

   if (sev->shaped) {
      UPWindowUpdateShape(up, upw);
   } else {
      UnityWindowTracker_ChangeWindowRegion(up->tracker, upw->toplevelWindow, NULL);
   }
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * UnityPlatformWindowProcessEvent --
 *
 *      Handle an event on a typical window.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Many.
 *
 *-----------------------------------------------------------------------------
 */

void
UPWindow_ProcessEvent(UnityPlatform *up,        // IN
                      UnityPlatformWindow *upw, // IN
                      Window realEventWindow,   // IN
                      const XEvent *xevent)     // IN
{
   Bool eventHandled = TRUE;

   ASSERT(up);
   ASSERT(upw);
   ASSERT(xevent);

   UPWindow_CheckRelevance(up, upw, xevent);

   switch (xevent->type) {
   case KeyPress:
   case KeyRelease:
   case ButtonPress:
   case ButtonRelease:
   case MotionNotify:
   case EnterNotify:
   case LeaveNotify:
   case KeymapNotify:
   case Expose:
   case GraphicsExpose:
   case NoExpose:
   case MapRequest:
   case ResizeRequest:
   case CirculateRequest:
   case SelectionClear:
   case SelectionRequest:
   case SelectionNotify:
   case ColormapNotify:
   case ClientMessage:
   case GravityNotify:
   case VisibilityNotify:
   case MappingNotify:
   case ReparentNotify:
   case ConfigureRequest:
      break; // No extra processing on these for now

   case CreateNotify:
      /* Do nothing. The UPWindow has already been created. */
      break;

   case FocusIn:
      if (upw->isRelevant) {
         UnityWindowInfo *info;
         info = UnityWindowTracker_LookupWindow(up->tracker, upw->toplevelWindow);

         UnityWindowTracker_ChangeWindowState(up->tracker,
                                              upw->toplevelWindow,
                                              info->state | UNITY_WINDOW_STATE_IN_FOCUS);
      }
      break;

   case FocusOut:
      if (upw->isRelevant) {
         UnityWindowInfo *info;
         info = UnityWindowTracker_LookupWindow(up->tracker, upw->toplevelWindow);

         UnityWindowTracker_ChangeWindowState(up->tracker,
                                              upw->toplevelWindow,
                                              (info->state
                                               & ~UNITY_WINDOW_STATE_IN_FOCUS));
      }
      break;

   case DestroyNotify:
      Debug("Destroying window (%p) %#lx/%#lx\n",
            upw, upw->toplevelWindow, upw->clientWindow);

      /*
       * Release the UnityPlatform object's reference to this UnityPlatformWindow,
       */
      upw->windowType = UNITY_WINDOW_TYPE_NONE;
      UPWindow_Unref(up, upw);
      break;

   case UnmapNotify:
      upw->wantInputFocus = FALSE;
      upw->isViewable = FALSE;
      break;

   case MapNotify:
      /*
       * This is here because we want to set input focus as part of RestoreWindow, but
       * can't call XSetInputFocus until the window has actually been shown.
       */
      if (upw->wantInputFocus && upw->clientWindow) {
         XSetInputFocus(up->display, upw->clientWindow, RevertToParent,
                        UnityPlatformGetServerTime(up));
         upw->wantInputFocus = FALSE;
      }

      upw->isViewable = TRUE;
      break;

   case CirculateNotify:
      if (upw->isRelevant) {
         UPWindow_Restack(up, upw,
                          (up->topWindow && xevent->xcirculate.place == PlaceOnTop)
                          ? up->topWindow->toplevelWindow : None);
      }
      break;

   case PropertyNotify:
      UPWindowProcessPropertyEvent(up, upw, xevent);
      break;

   case ConfigureNotify:
      UPWindowProcessConfigureEvent(up, upw, xevent);
      break;

   default:
      eventHandled = FALSE; // Unknown "regular" event
      break;
   }

   if (!eventHandled) { // Handle extension events here
#if defined(VM_HAVE_X11_SHAPE_EXT)
      if (up->shapeEventBase &&
          xevent->type == (up->shapeEventBase + ShapeNotify)) {
         UPWindowProcessShapeEvent(up, upw, xevent);
         eventHandled = TRUE;
      }
#endif

      ASSERT(eventHandled);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowUpdateTitle --
 *
 *      Tells the window tracker about the window's latest title
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Notification to the window tracker
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowUpdateTitle(UnityPlatform *up,        // IN
                    UnityPlatformWindow *upw) // IN
{
   Atom propertyType;
   int propertyFormat;
   unsigned long itemsReturned;
   unsigned long bytesRemaining;
   unsigned char *valueReturned = NULL;
   DynBuf titleBuf;

   if (!upw->clientWindow) {
      return;
   }

   if (XGetWindowProperty(up->display, upw->clientWindow, up->atoms.WM_NAME, 0,
                          1024, False, AnyPropertyType,
                          &propertyType, &propertyFormat, &itemsReturned,
                          &bytesRemaining, &valueReturned)
       != Success) {
      /*
       * Some random error occurred - perhaps the window disappeared
       */
      return;
   }

   if (propertyType != XA_STRING || propertyFormat != 8) {
      return;
   }

   DynBuf_Init(&titleBuf);
   DynBuf_Append(&titleBuf, valueReturned, itemsReturned);
   if (!itemsReturned || valueReturned[itemsReturned-1] != '\0') {
      DynBuf_AppendString(&titleBuf, "");
   }
   XFree(valueReturned);
   Debug("Set title of window %#lx to %s\n",
         upw->clientWindow, (char *)DynBuf_Get(&titleBuf));
   UnityWindowTracker_SetWindowTitle(up->tracker, (UnityWindowId) upw->toplevelWindow,
                                     &titleBuf);
   DynBuf_Destroy(&titleBuf);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowUpdateType --
 *
 *      Tells the window tracker about the window's latest type
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Notification to the window tracker
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowUpdateType(UnityPlatform *up,        // IN
                   UnityPlatformWindow *upw) // IN
{
   ASSERT(up);
   ASSERT(upw);

   /*
    * upw->windowType was previously updated by the CheckRelevance method.
    */
   UnityWindowTracker_ChangeWindowType(up->tracker,
                                       upw->toplevelWindow,
                                       upw->windowType);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowUpdateProtocols --
 *
 *      Updates the list of protocols supported by the window.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Notification to the window tracker
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowUpdateProtocols(UnityPlatform *up,        // IN
                        UnityPlatformWindow *upw) // IN
{
   Atom propertyType;
   int propertyFormat;
   unsigned long itemsReturned;
   unsigned long bytesRemaining;
   Atom *valueReturned = NULL;
   int i;

   if (!upw->clientWindow) {
      return;
   }

   if (XGetWindowProperty(up->display, upw->clientWindow, up->atoms.WM_STATE, 0,
                          1024, False, AnyPropertyType,
                          &propertyType, &propertyFormat, &itemsReturned,
                          &bytesRemaining, (unsigned char **) &valueReturned)
       != Success) {
      /*
       * Some random error occurred - perhaps the window disappeared.
       */
      return;
   }

   if (propertyType != up->atoms.WM_STATE
       || propertyFormat != 32) {
      itemsReturned = 0;
   }

   memset(upw->windowProtocols, 0, sizeof upw->windowProtocols);
   for (i = 0; i < itemsReturned; i++) {
      UnityX11WinProtocol proto;

      if (valueReturned[i] == up->atoms.WM_DELETE_WINDOW) {
         proto = UNITY_X11_WIN_WM_DELETE_WINDOW;
      } else {
         continue;
      }

      upw->windowProtocols[proto] = TRUE;
   }
   XFree(valueReturned);
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowUpdateActions --
 *
 *      Updates the window attributes based on a new _NET_WM_ALLOWED_ACTIONS attribute.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Notification to the window tracker
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowUpdateActions(UnityPlatform *up,        // IN
                      UnityPlatformWindow *upw) // IN
{
   Atom propertyType;
   int propertyFormat;
   unsigned long itemsReturned = 0;
   unsigned long bytesRemaining;
   Atom *valueReturned = NULL;
   int i;
   Bool curAttrValues[UNITY_MAX_ATTRIBUTES];
   Bool attrsAreSet[UNITY_MAX_ATTRIBUTES];
   Bool haveHorizMax;
   Bool haveVertMax;

   if (!upw->clientWindow) {
      return;
   }

   memset(curAttrValues, 0, sizeof curAttrValues);
   memset(attrsAreSet, 0, sizeof attrsAreSet);
   haveHorizMax = haveVertMax = FALSE;

   /*
    * List of attributes that we know how to process from the ALLOWED_ACTIONS list. If we
    * don't find these in the ALLOWED_ACTIONS list, and they are supported by the window
    * manager, then we report that they're FALSE (set by the first memset above).
    */
   attrsAreSet[UNITY_WINDOW_ATTR_MINIMIZABLE] =
      UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_WM_ACTION_MINIMIZE);
   attrsAreSet[UNITY_WINDOW_ATTR_MAXIMIZABLE] =
      UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_WM_ACTION_MAXIMIZE_HORZ) &&
      UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_WM_ACTION_MAXIMIZE_VERT);
   attrsAreSet[UNITY_WINDOW_ATTR_CLOSABLE] =
      UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_WM_ACTION_CLOSE);
   attrsAreSet[UNITY_WINDOW_ATTR_FULLSCREENABLE] =
      UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_WM_ACTION_FULLSCREEN);
   attrsAreSet[UNITY_WINDOW_ATTR_SHADEABLE] =
      UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_WM_ACTION_SHADE);
   attrsAreSet[UNITY_WINDOW_ATTR_STICKABLE] =
      UnityPlatformWMProtocolSupported(up, UNITY_X11_WM__NET_WM_ACTION_STICK);

   if (XGetWindowProperty(up->display, upw->clientWindow,
                          up->atoms._NET_WM_ALLOWED_ACTIONS, 0,
                          1024, False, XA_ATOM,
                          &propertyType, &propertyFormat, &itemsReturned,
                          &bytesRemaining, (unsigned char **) &valueReturned)
       == Success
      && propertyFormat == 32) {
      for (i = 0; i < itemsReturned; i++) {
         UnityWindowAttribute attr;
         Bool attrValue = TRUE;

         if (valueReturned[i] == up->atoms._NET_WM_ACTION_MINIMIZE) {
            attr = UNITY_WINDOW_ATTR_MINIMIZABLE;
         } else if (valueReturned[i] == up->atoms._NET_WM_ACTION_MAXIMIZE_HORZ) {
            haveHorizMax = TRUE;
            continue;
         } else if (valueReturned[i] == up->atoms._NET_WM_ACTION_MAXIMIZE_VERT) {
            haveVertMax = TRUE;
            continue;
         } else if (valueReturned[i] == up->atoms._NET_WM_ACTION_CLOSE) {
            attr = UNITY_WINDOW_ATTR_CLOSABLE;
         } else if (valueReturned[i] == up->atoms._NET_WM_ACTION_FULLSCREEN) {
            attr = UNITY_WINDOW_ATTR_FULLSCREENABLE;
         } else if (valueReturned[i] == up->atoms._NET_WM_ACTION_SHADE) {
            attr = UNITY_WINDOW_ATTR_SHADEABLE;
         } else if (valueReturned[i] == up->atoms._NET_WM_ACTION_STICK) {
            attr = UNITY_WINDOW_ATTR_STICKABLE;
         } else {
            continue;
         }

         curAttrValues[attr] = attrValue;
         attrsAreSet[attr] = TRUE;
      }
      XFree(valueReturned);
   } else {
      curAttrValues[UNITY_WINDOW_ATTR_MINIMIZABLE] = TRUE;
      attrsAreSet[UNITY_WINDOW_ATTR_MINIMIZABLE] = TRUE;
   }

   curAttrValues[UNITY_WINDOW_ATTR_MAXIMIZABLE] = (haveHorizMax && haveVertMax);
   attrsAreSet[UNITY_WINDOW_ATTR_MAXIMIZABLE] = TRUE;

   for (i = 0; i < UNITY_MAX_ATTRIBUTES; i++) {
      if (attrsAreSet[i]) {
         UnityWindowTracker_ChangeWindowAttribute(up->tracker, upw->toplevelWindow,
                                                  i, curAttrValues[i]);
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowGetDesktop --
 *
 *      Retrieve's the current X11 virtual desktop of a window.
 *
 * Results:
 *      Virtual desktop number
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
UPWindowGetDesktop(UnityPlatform *up,        // IN
                   UnityPlatformWindow *upw, // IN
                   int *guestDesktop)        // IN/OUT
{
   Atom propertyType;
   int propertyFormat;
   unsigned long itemsReturned = 0;
   unsigned long bytesRemaining;
   Atom *valueReturned = NULL;
   Bool retval = FALSE;

   if (!upw->clientWindow) {
      return FALSE;
   }

   if (XGetWindowProperty(up->display, upw->clientWindow,
                          up->atoms._NET_WM_DESKTOP, 0,
                          1024, False, AnyPropertyType,
                          &propertyType, &propertyFormat, &itemsReturned,
                          &bytesRemaining, (unsigned char **) &valueReturned)
       == Success
       && propertyType == XA_CARDINAL
       && propertyFormat == 32
       && itemsReturned) {

      *guestDesktop = *valueReturned;
      retval = TRUE;
   }

   XFree(valueReturned);

   return retval;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowUpdateDesktop --
 *
 *      Updates the window's virtual desktop based on a new _NET_WM_DESKTOP attribute.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Notification to the window tracker.
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowUpdateDesktop(UnityPlatform *up,        // IN
                      UnityPlatformWindow *upw) // IN
{
   int guestDesktop = -1;

   if (!upw->clientWindow) {
      return;
   }

   if (!UPWindowGetDesktop(up, upw, &guestDesktop)) {
      Debug("Window %#lx has a clientWindow, but its "
            "virtual desktop could not be retrieved\n",
            upw->clientWindow);
      return;
   }

   if (guestDesktop < ((int)up->desktopInfo.numDesktops)) {
      UnityDesktopId desktopId = -1;
      Bool isSticky;

      isSticky = (guestDesktop < 0);
      if (!isSticky) {
         desktopId = up->desktopInfo.guestDesktopToUnity[guestDesktop];
      }

      UnityWindowTracker_ChangeWindowDesktop(up->tracker,
                                             upw->toplevelWindow,
                                             desktopId);

      UnityWindowTracker_ChangeWindowAttribute(up->tracker,
                                               upw->toplevelWindow,
                                               UNITY_WINDOW_ATTR_STICKY,
                                               isSticky);
   } else {
      Debug("Guest's virtual desktop config may not match host's (yet?)"
            " (window is on desktop %d, guest is supposed to have %"FMTSZ"u desktops)\n",
            guestDesktop, up->desktopInfo.numDesktops);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowUpdateIcon --
 *
 *      Updates the window's virtual desktop based on a new _NET_WM_DESKTOP attribute.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Notification to the window tracker.
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowUpdateIcon(UnityPlatform *up,        // IN
                   UnityPlatformWindow *upw) // IN
{
   UnityWindowTracker_NotifyIconChanged(up->tracker, upw->toplevelWindow,
                                        UNITY_ICON_TYPE_MAIN);

   if (DynBuf_GetSize(&upw->iconPng.data)) {
      DynBuf_SetSize(&upw->iconPng.data, 0);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowUpdateState --
 *
 *      Tells the window tracker about the window's changes to the _NET_WM_STATE and
 *      WM_STATE properties.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Notification to the window tracker
 *
 *-----------------------------------------------------------------------------
 */

static void
UPWindowUpdateState(UnityPlatform *up,        // IN
                    UnityPlatformWindow *upw) // IN
{
   Atom propertyType;
   int propertyFormat;
   unsigned long itemsReturned;
   unsigned long bytesRemaining;
   Atom *valueReturned = NULL;
   int i;
   Bool curAttrValues[UNITY_MAX_ATTRIBUTES];
   Bool attrsAreSet[UNITY_MAX_ATTRIBUTES];
   Bool isMinimized = FALSE;
   Bool haveHorizMax = FALSE;
   Bool haveVertMax = FALSE;
   Bool doSkipTaskbar = FALSE;
   Bool doSkipPager = FALSE;
   Window mainWindow;

   mainWindow = upw->clientWindow ? upw->clientWindow : upw->toplevelWindow;

   memset(curAttrValues, 0, sizeof curAttrValues);
   memset(attrsAreSet, 0, sizeof attrsAreSet);

   curAttrValues[UNITY_WINDOW_ATTR_VISIBLE] = TRUE;
   attrsAreSet[UNITY_WINDOW_ATTR_VISIBLE] =
      attrsAreSet[UNITY_WINDOW_ATTR_MAXIMIZED] =
      attrsAreSet[UNITY_WINDOW_ATTR_STICKY] =
      attrsAreSet[UNITY_WINDOW_ATTR_ALWAYS_ABOVE] =
      attrsAreSet[UNITY_WINDOW_ATTR_ALWAYS_BELOW] =
      attrsAreSet[UNITY_WINDOW_ATTR_MODAL] =
      attrsAreSet[UNITY_WINDOW_ATTR_SHADED] =
      attrsAreSet[UNITY_WINDOW_ATTR_FULLSCREENED] =
      attrsAreSet[UNITY_WINDOW_ATTR_ATTN_WANTED] = TRUE;

   if (XGetWindowProperty(up->display, mainWindow, up->atoms.WM_STATE, 0,
                          1024, False, AnyPropertyType,
                          &propertyType, &propertyFormat, &itemsReturned,
                          &bytesRemaining, (unsigned char **) &valueReturned)
       != Success) {
      /*
       * Some random error occurred - perhaps the window disappeared
       */
      return;
   }

   if (propertyType == up->atoms.WM_STATE
       && propertyFormat == 32
       && itemsReturned
       && valueReturned[0] == IconicState) {
      isMinimized = TRUE;
   }
   XFree(valueReturned);

   if (XGetWindowProperty(up->display, mainWindow, up->atoms._NET_WM_STATE, 0,
                          1024, False, AnyPropertyType,
                          &propertyType, &propertyFormat, &itemsReturned,
                          &bytesRemaining, (unsigned char **) &valueReturned)
       != Success) {
      /*
       * Some random error occurred - perhaps the window disappeared
       */
      return;
   }

   if (propertyType != XA_ATOM
       || propertyFormat != 32) {
      itemsReturned = 0;
   }

   for (i = 0; i < itemsReturned; i++) {
      UnityWindowAttribute attr;
      Bool attrValue = TRUE;

      if (valueReturned[i] == up->atoms._NET_WM_STATE_MINIMIZED
          || valueReturned[i] == up->atoms._NET_WM_STATE_HIDDEN) {
         /*
          * Unfortunately, the HIDDEN attribute is used by some WM's to mean
          * "minimized" when they should really be separate.
          */
         isMinimized = TRUE;
         continue;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_MAXIMIZED_HORZ) {
         haveHorizMax = TRUE;
         continue;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_MAXIMIZED_VERT) {
         haveVertMax = TRUE;
         continue;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_STICKY) {
         attr = UNITY_WINDOW_ATTR_STICKY;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_ABOVE) {
         attr = UNITY_WINDOW_ATTR_ALWAYS_ABOVE;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_BELOW) {
         attr = UNITY_WINDOW_ATTR_ALWAYS_BELOW;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_MODAL) {
         attr = UNITY_WINDOW_ATTR_MODAL;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_SHADED) {
         attr = UNITY_WINDOW_ATTR_SHADED;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_FULLSCREEN) {
         attr = UNITY_WINDOW_ATTR_FULLSCREENED;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_DEMANDS_ATTENTION) {
         attr = UNITY_WINDOW_ATTR_ATTN_WANTED;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_SKIP_TASKBAR) {
         attr = UNITY_WINDOW_ATTR_TOOLWINDOW;
         doSkipTaskbar = TRUE;
      } else if (valueReturned[i] == up->atoms._NET_WM_STATE_SKIP_PAGER) {
         doSkipPager = TRUE;
         continue;
      } else {
         continue;
      }

      curAttrValues[attr] = attrValue;
      attrsAreSet[attr] = TRUE;
   }
   XFree(valueReturned);

   curAttrValues[UNITY_WINDOW_ATTR_MAXIMIZED] = (haveHorizMax && haveVertMax);
   attrsAreSet[UNITY_WINDOW_ATTR_MAXIMIZED] = TRUE;
   curAttrValues[UNITY_WINDOW_ATTR_APPWINDOW] = (!doSkipPager && !doSkipTaskbar)
      && (upw->windowType == UNITY_WINDOW_TYPE_NORMAL);
   attrsAreSet[UNITY_WINDOW_ATTR_APPWINDOW] = TRUE;

   if (upw->isRelevant) {
      UnityWindowInfo *info;
      uint32 newState;
      info = UnityWindowTracker_LookupWindow(up->tracker, upw->toplevelWindow);
      ASSERT(info);

      newState = info->state;
      if (isMinimized) {
         newState |= UNITY_WINDOW_STATE_MINIMIZED;
      } else {
         newState &= ~UNITY_WINDOW_STATE_MINIMIZED;
      }

      UnityWindowTracker_ChangeWindowState(up->tracker,
                                           upw->toplevelWindow,
                                           newState);

      upw->isMinimized = isMinimized;
      upw->isMaximized = (haveHorizMax && haveVertMax);

      for (i = 0; i < UNITY_MAX_ATTRIBUTES; i++) {
         if (attrsAreSet[i]) {
            UnityWindowTracker_ChangeWindowAttribute(up->tracker, upw->toplevelWindow,
                                                     i, curAttrValues[i]);
         }
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindowPushFullUpdate --
 *
 *      Pushes a full update for the given window.
 *
 * Results:
 *      TRUE if we pushed the updates for this window, FALSE otherwise.
 *
 * Side effects:
 *      UnityWindowTracker contains latest info on this window.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
UPWindowPushFullUpdate(UnityPlatform *up,            // IN
                       UnityPlatformWindow *upw)     // IN
{
   XWindowAttributes winAttr;
   Atom *props;
   int propCount;
   int i;

   XGetWindowAttributes(up->display, upw->toplevelWindow, &winAttr);

   UnityWindowTracker_MoveWindow(up->tracker, (UnityWindowId) upw->toplevelWindow,
                                 winAttr.x - winAttr.border_width,
                                 winAttr.y - winAttr.border_width,
                                 winAttr.x + winAttr.width + winAttr.border_width,
                                 winAttr.y + winAttr.height + winAttr.border_width);

#if defined(VM_HAVE_X11_SHAPE_EXT)
   UPWindowUpdateShape(up, upw);
#endif

   propCount = 0;
   UnityPlatformResetErrorCount(up);
   props = XListProperties(up->display,
                           upw->clientWindow ? upw->clientWindow : upw->toplevelWindow,
                           &propCount);
   if (!UnityPlatformGetErrorCount(up)) {
      for (i = 0; i < propCount; i++) {
         XEvent fakeEvent;

         fakeEvent.xproperty.atom = props[i];
         UPWindowProcessPropertyEvent(up, upw, &fakeEvent);
      }
      XFree(props);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * UPWindow_ProtocolSupported --
 *
 *      Returns whether a particular window supports a particular protocol.
 *
 * Results:
 *      TRUE if supported, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
UPWindow_ProtocolSupported(const UnityPlatform *up,        // IN
                           const UnityPlatformWindow *upw, // IN
                           UnityX11WinProtocol proto)      // IN
{
   ASSERT(up);
   ASSERT(upw);
   ASSERT(proto < UNITY_X11_MAX_WIN_PROTOCOLS);

   return upw->windowProtocols[proto];
}


/*
 *----------------------------------------------------------------------------
 *
 * UnityPlatformShowWindow --
 *
 *      Makes hidden Window visible. If the Window is already visible, it stays
 *      visible. Window reappears at its original location. A minimized window
 *      reappears as minimized.
 *
 * Results:
 *
 *      FALSE if the Window handle is invalid.
 *      TRUE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
UnityPlatformShowWindow(UnityPlatform *up,    // IN
                        UnityWindowId window) // IN
{
   UnityPlatformWindow *upw;

   ASSERT(up);

   upw = UPWindow_Lookup(up, window);

   if (!upw || !upw->clientWindow) {
      Debug("Hiding FAILED!\n");
      return FALSE;
   }

   if (upw->isHidden) {
      Atom data[5] = {0, 0, 0, 0, 0};

      /*
       * Unfortunately the _NET_WM_STATE messages only work for windows that are already
       * mapped, i.e. not iconified or withdrawn.
       */
      if (!upw->isMinimized) {
         XMapRaised(up->display, upw->clientWindow);
      }

      data[0] = _NET_WM_STATE_REMOVE;
      data[1] = up->atoms._NET_WM_STATE_HIDDEN;
      data[3] = 2; // Message is from the pager/taskbar
      UnityPlatformSendClientMessage(up, upw->rootWindow, upw->clientWindow,
                                     up->atoms._NET_WM_STATE, 32, 4, data);

      upw->wantInputFocus = TRUE;
      upw->isHidden = FALSE;
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * UnityPlatformHideWindow --
 *
 *      Hides window. If the window is already hidden it stays hidden. Hides
 *      maximized and minimized windows too.
 *
 * Results:
 *      FALSE if the Window handle is invalid.
 *      TRUE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
UnityPlatformHideWindow(UnityPlatform *up,    // IN
                        UnityWindowId window) // IN
{
   UnityPlatformWindow *upw;

   ASSERT(up);

   upw = UPWindow_Lookup(up, window);

   if (!upw
      || !upw->clientWindow) {
      Debug("Hiding FAILED!\n");
      return FALSE;
   }

   if (!upw->isHidden) {
      Atom data[5] = {0, 0, 0, 0, 0};

      upw->isHidden = TRUE;

      data[0] = _NET_WM_STATE_ADD;
      data[1] = up->atoms._NET_WM_STATE_HIDDEN;
      data[3] = 2; // Message is from a pager/taskbar/etc.
      UnityPlatformSendClientMessage(up, upw->rootWindow, upw->clientWindow,
                                     up->atoms._NET_WM_STATE, 32, 4, data);
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * UnityPlatformMinimizeWindow --
 *
 *      Mimimizes window. If the window is already mimimized it stays minimized.
 *
 * Results:
 *      FALSE if the Window handle is invalid.
 *      TRUE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
UnityPlatformMinimizeWindow(UnityPlatform *up,    // IN
                            UnityWindowId window) // IN
{
   UnityPlatformWindow *upw;

   ASSERT(up);

   upw = UPWindow_Lookup(up, window);

   if (!upw
      || !upw->clientWindow) {
      Debug("Minimizing FAILED!\n");
      return FALSE;
   }

   Debug("UnityPlatformMinimizeWindow(%#lx)\n", upw->toplevelWindow);
   upw->wantInputFocus = FALSE;
   if (!upw->isMinimized) {
      Atom data[5] = {0, 0, 0, 0, 0};

      upw->isMinimized = TRUE;
      data[0] = _NET_WM_STATE_ADD;
      data[1] = up->atoms._NET_WM_STATE_MINIMIZED;
      data[3] = 2; // Message is from a pager/taskbar/etc.
      UnityPlatformSendClientMessage(up, upw->rootWindow, upw->clientWindow,
                                     up->atoms._NET_WM_STATE, 32, 4, data);

      XIconifyWindow(up->display, upw->clientWindow, 0);
   }


   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * UnityPlatformMaximizeWindow --
 *
 *      Maximizes window. If the window is already maximized it will stay so.
 *
 * Results:
 *      FALSE if the Window handle is invalid.
 *      TRUE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
UnityPlatformMaximizeWindow(UnityPlatform *up,    // IN
                            UnityWindowId window) // IN
{
   UnityPlatformWindow *upw;

   ASSERT(up);

   upw = UPWindow_Lookup(up, window);

   if (!upw
      || !upw->clientWindow) {
      Debug("Maximizing FAILED!\n");
      return FALSE;
   }

   if (!upw->isMaximized) {
      Atom data[5] = {0, 0, 0, 0, 0};

      upw->isMaximized = TRUE;
      data[0] = _NET_WM_STATE_ADD;
      data[1] = up->atoms._NET_WM_STATE_MAXIMIZED_HORZ;
      data[2] = up->atoms._NET_WM_STATE_MAXIMIZED_VERT;
      data[3] = 2; // Message is from a pager/taskbar/etc.
      UnityPlatformSendClientMessage(up, upw->rootWindow, upw->clientWindow,
                                     up->atoms._NET_WM_STATE, 32, 4, data);
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * UnityPlatformUnmaximizeWindow --
 *
 *      Unmaximizes window. If the window is already unmaximized, it will stay
 *      that way.
 *
 * Results:
 *      FALSE if the Window handle is invalid.
 *      TRUE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
UnityPlatformUnmaximizeWindow(UnityPlatform *up,    // IN
                              UnityWindowId window) // IN
{
   UnityPlatformWindow *upw;

   ASSERT(up);

   upw = UPWindow_Lookup(up, window);

   if (!upw
      || !upw->clientWindow) {
      Debug("Maximizing FAILED!\n");
      return FALSE;
   }

   if (upw->isMaximized) {
      Atom data[5] = {0, 0, 0, 0, 0};

      data[0] = _NET_WM_STATE_REMOVE;
      data[1] = up->atoms._NET_WM_STATE_MAXIMIZED_HORZ;
      data[2] = up->atoms._NET_WM_STATE_MAXIMIZED_VERT;
      data[3] = 2; // Message is from a pager/taskbar/etc.
      UnityPlatformSendClientMessage(up, upw->rootWindow, upw->clientWindow,
                                     up->atoms._NET_WM_STATE, 32, 4, data);

      upw->isMaximized = FALSE;
   }

   return TRUE;
}


/*
 *------------------------------------------------------------------------------
 *
 * UnityPlatformSetWindowDesktop --
 *
 *     Move the window to the specified desktop. The desktopId is an index
 *     into the desktop configuration array.
 *
 * Results:
 *     Returns TRUE if successful, and FALSE otherwise.
 *
 * Side effects:
 *     None.
 *
 *------------------------------------------------------------------------------
 */

Bool
UnityPlatformSetWindowDesktop(UnityPlatform *up,         // IN
                              UnityWindowId windowId,    // IN
                              UnityDesktopId desktopId)  // IN
{
   UnityPlatformWindow *upw;
   Atom data[5] = {0, 0, 0, 0, 0};
   uint32 guestDesktopId;

   ASSERT(up);

   upw = UPWindow_Lookup(up, windowId);

   if (!upw
      || !upw->clientWindow) {
      Debug("Desktop change FAILED on %#lx (perhaps it has no clientWindow)!\n",
            upw ? upw->toplevelWindow : 0);
      return FALSE;
   }

   /*
    * XXX I wrote this code assuming that UnityWindowTracker on the
    * guest side will be updated with the latest settings as they come
    * from the host, but that is not currently the case. unity.c needs
    * fixing.
    */

   ASSERT(desktopId < up->desktopInfo.numDesktops);
   guestDesktopId = up->desktopInfo.unityDesktopToGuest[desktopId];

   if (!upw->isViewable) {
     Atom currentDesktop = guestDesktopId; // Cast for 64-bit correctness.

     /*
      * Sending the _NET_WM_DESKTOP client message only works if the
      * window is mapped. We should still send that message to
      * eliminate race conditions, but if the window is not mapped, we
      * also need to set the property on the window so that it shows
      * up on the correct desktop when it is re-mapped.
      */

     XChangeProperty(up->display, (Window)upw->clientWindow, up->atoms._NET_WM_DESKTOP,
		     XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&currentDesktop, 1);
   }

   data[0] = guestDesktopId;
   data[1] = 2; // Indicates that this was requested by the pager/taskbar/etc.
   UnityPlatformSendClientMessage(up,
				  upw->rootWindow,
				  upw->clientWindow,
				  up->atoms._NET_WM_DESKTOP,
				  32, 5, data);

   return TRUE;
}