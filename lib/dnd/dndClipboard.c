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
 * dndClipboard.c
 *
 *      Implements dndClipboard.h
 */

#include <stdlib.h>
#include <string.h>

#include "vm_assert.h"

#include "dndClipboard.h"
#include "dndInt.h"


#define CPFormatToIndex(x) ((unsigned int)(x) - 1)

/*
 *----------------------------------------------------------------------------
 *
 * CPClipItemInit --
 *
 *      CPClipboardItem constructor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
CPClipItemInit(CPClipItem *item)        // IN/OUT: the clipboard item
{
   ASSERT(item);

   item->buf = NULL;
   item->size = 0;
   item->exists = FALSE;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipItemDestroy --
 *
 *      CPCilpboardItem destructor.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

static INLINE void
CPClipItemDestroy(CPClipItem *item)     // IN/OUT: the clipboard item
{
   ASSERT(item);

   free(item->buf);
   item->buf = NULL;
   item->size = 0;
   item->exists = FALSE;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipItemCopy --
 *
 *      Copy clipboard item from src to dest.
 *
 * Results:
 *      TRUE on success, FALSE on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipItemCopy(CPClipItem *dest,        // IN: dest clipboard item
               const CPClipItem *src)   // IN: source clipboard item
{
   ASSERT(dest);
   ASSERT(src);

   if (src->buf) {
      void *tmp = dest->buf;
      dest->buf = realloc(dest->buf, src->size);
      if (!dest->buf) {
         dest->buf = tmp;
         return FALSE;
      }
      memcpy(dest->buf, src->buf, src->size);
   }

   dest->size = src->size;
   dest->exists = src->exists;

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_Init --
 *
 *      Constructor for CPClipboard.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

void
CPClipboard_Init(CPClipboard *clip)     // IN/OUT: the clipboard
{
   unsigned int i;

   ASSERT(clip);

   clip->changed = TRUE;
   for (i = CPFORMAT_MIN; i < CPFORMAT_MAX; ++i) {
      CPClipItemInit(&clip->items[CPFormatToIndex(i)]);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_Destroy --
 *
 *      Destroys a clipboard.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

void
CPClipboard_Destroy(CPClipboard *clip)  // IN/OUT: the clipboard
{
   unsigned int i;

   ASSERT(clip);

   for (i = CPFORMAT_MIN; i < CPFORMAT_MAX; ++i) {
      CPClipItemDestroy(&clip->items[CPFormatToIndex(i)]);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_Clear --
 *
 *      Clears the items in CPClipboard.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

void
CPClipboard_Clear(CPClipboard *clip)    // IN/OUT: the clipboard
{
   unsigned int i;

   ASSERT(clip);

   clip->changed = TRUE;
   for (i = CPFORMAT_MIN; i < CPFORMAT_MAX; ++i) {
      CPClipboard_ClearItem(clip, i);
   }
}


/*
 *----------------------------------------------------------------------------
 *
 *  CPClipboard_SetItem --
 *
 *      Makes a copy of the item adds it to the clipboard. If something
 *      already exists for the format it is overwritten. To set a promised
 *      type, pass in NULL for buffer and 0 for the size.
 *
 * Results:
 *      TRUE on success, FALSE on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipboard_SetItem(CPClipboard *clip,          // IN/OUT: the clipboard
                    const DND_CPFORMAT fmt,     // IN: the item format
                    const void *clipitem,       // IN: the item
                    const size_t size)          // IN: the item size
{
   CPClipItem *item;
   void *newBuf = NULL;

   ASSERT(clip);

   if (!(CPFORMAT_UNKNOWN < fmt && fmt < CPFORMAT_MAX)) {
      return FALSE;
   }

   if (!CPClipboard_ClearItem(clip, fmt)) {
      return FALSE;
   }

   item = &clip->items[CPFormatToIndex(fmt)];

   if (clipitem) {
      newBuf = malloc(size);
      if (!newBuf) {
         return FALSE;
      }
      memcpy(newBuf, clipitem, size);
   }

   item->buf = newBuf;
   item->size = size;
   item->exists = TRUE;

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_ClearItem --
 *
 *      Clears the item in the clipboard.
 *
 * Results:
 *      TRUE on success, FALSE on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipboard_ClearItem(CPClipboard *clip,        // IN: the clipboard
                      DND_CPFORMAT fmt)         // IN: item to be cleared
{
   CPClipItem *item;

   ASSERT(clip);

   if (!(CPFORMAT_UNKNOWN < fmt && fmt < CPFORMAT_MAX)) {
      return FALSE;
   }

   item = &clip->items[CPFormatToIndex(fmt)];

   free(item->buf);
   item->buf = NULL;
   item->size = 0;
   item->exists = FALSE;

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_GetItem  --
 *
 *      Get the clipboard item of format fmt from the clipboard. The clipboard
 *      maintains ownership of the data. If the item is promised, the buffer
 *      will contain NULL and the size will be 0.
 *
 * Results:
 *      TRUE if item exists, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipboard_GetItem(const CPClipboard *clip,    // IN: the clipboard
                    DND_CPFORMAT fmt,           // IN: the format
                    void **buf,                 // OUT
                    size_t *size)               // OUT
{
   ASSERT(clip);
   ASSERT(size);
   ASSERT(buf);

   if(!(CPFORMAT_UNKNOWN < fmt && fmt < CPFORMAT_MAX)) {
      return FALSE;
   }

   if (clip->items[CPFormatToIndex(fmt)].exists) {
      *buf = clip->items[CPFormatToIndex(fmt)].buf;
      *size = clip->items[CPFormatToIndex(fmt)].size;
      return TRUE;
   } else {
      ASSERT(!clip->items[CPFormatToIndex(fmt)].size);
      return FALSE;
   }
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_ItemExists  --
 *
 *      Check if the clipboard item of format fmt exists or not.
 *
 * Results:
 *      TRUE if item exists, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipboard_ItemExists(const CPClipboard *clip,    // IN: the clipboard
                       DND_CPFORMAT fmt)           // IN: the format
{
   ASSERT(clip);

   if(!(CPFORMAT_UNKNOWN < fmt && fmt < CPFORMAT_MAX)) {
      return FALSE;
   }

   return (clip->items[CPFormatToIndex(fmt)].exists &&
           clip->items[CPFormatToIndex(fmt)].size > 0);
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_IsEmpty  --
 *
 *      Check if the clipboard item of format fmt exists or not.
 *
 * Results:
 *      TRUE if item exists, FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipboard_IsEmpty(const CPClipboard *clip)    // IN: the clipboard
{
   unsigned int i;

   ASSERT(clip);

   for (i = CPFORMAT_MIN; i < CPFORMAT_MAX; ++i) {
      if (clip->items[CPFormatToIndex(i)].exists &&
          clip->items[CPFormatToIndex(i)].size > 0) {
         return FALSE;
      }
   }
   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_SetChanged  --
 *
 *      Set clip->changed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

void
CPClipboard_SetChanged(CPClipboard *clip, // IN/OUT: the clipboard
                       Bool changed)      // IN
{
   clip->changed = changed;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_Changed  --
 *
 *      Return clip->changed.
 *
 * Results:
 *      Return clip->changed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipboard_Changed(const CPClipboard *clip)    // IN: the clipboard
{
   return clip->changed;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_Copy --
 *
 *      Copies the clipboard contents from src to dest. Assumes that dest has
 *      been initialized and contains no data.
 *
 * Results:
 *      TRUE on success, FALSE on failure. On failure the caller should
 *      destroy the object to ensure memory is cleaned up.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipboard_Copy(CPClipboard *dest,             // IN: the desination clipboard
                 const CPClipboard *src)        // IN: the source clipboard
{
   unsigned int i;

   ASSERT(dest);
   ASSERT(src);

   for (i = CPFORMAT_MIN; i < CPFORMAT_MAX; ++i) {
      if (!CPClipItemCopy(&dest->items[CPFormatToIndex(i)],
                         &src->items[CPFormatToIndex(i)])) {
         return FALSE;
      }
   }
   dest->changed = src->changed;

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_Serialize --
 *
 *      Serialize the contents of the CPClipboard out to the provided dynbuf.
 *
 * Results:
 *      TRUE on success.
 *      FALSE on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipboard_Serialize(const CPClipboard *clip, // IN
                      DynBuf *buf)             // OUT: the output buffer
{
   DND_CPFORMAT fmt;
   uint32 maxFmt = CPFORMAT_MAX;

   ASSERT(clip);
   ASSERT(buf);

   /* First append number of formats in clip. */
   if (!DynBuf_Append(buf, &maxFmt, sizeof maxFmt)) {
      return FALSE;
   }

   /* Append format data one by one. */
   for (fmt = CPFORMAT_MIN; fmt < CPFORMAT_MAX; ++fmt) {
      CPClipItem *item = (CPClipItem *)&(clip->items[CPFormatToIndex(fmt)]);
      if (!DynBuf_Append(buf, &item->exists, sizeof item->exists) ||
          !DynBuf_Append(buf, &item->size, sizeof item->size)) {
         return FALSE;
      }
      if (item->exists && (item->size > 0) &&
          !DynBuf_Append(buf, item->buf, item->size)) {
         return FALSE;
      }
   }

   if (!DynBuf_Append(buf, &clip->changed, sizeof clip->changed)) {
      return FALSE;
   }

   return TRUE;
}


/*
 *----------------------------------------------------------------------------
 *
 * CPClipboard_Unserialize --
 *
 *      Unserialize the arguments of the CPClipboard provided by the buffer.
 *      On failure the clip will be destroyed.
 *
 * Results:
 *      TRUE if success, FALSE otherwise
 *
 * Side effects:
 *      The clip passed in should be empty, otherwise will cause memory leakage.
 *      On success, arguments found in buf are unserialized into clip.
 *
 *----------------------------------------------------------------------------
 */

Bool
CPClipboard_Unserialize(CPClipboard *clip, // OUT: the clipboard
                        void *buf,         // IN: input buffer
                        size_t len)        // IN: buffer length
{
   DND_CPFORMAT fmt;
   BufRead r;
   uint32 maxFmt;

   ASSERT(clip);
   ASSERT(buf);

   CPClipboard_Init(clip);

   r.pos = buf;
   r.unreadLen = len;

   /* First get number of formats in the buf. */
   if (!DnDReadBuffer(&r, &maxFmt, sizeof maxFmt)) {
      goto error;
   }

   /* This version only supports number of formats up to CPFORMAT_MAX. */
   maxFmt = MIN(CPFORMAT_MAX, maxFmt);

   for (fmt = CPFORMAT_MIN; fmt < maxFmt; ++fmt) {
      Bool exists;
      uint32 size;

      if (!DnDReadBuffer(&r, &exists, sizeof exists) ||
          !DnDReadBuffer(&r, &size, sizeof size)) {
         goto error;
      }

      if (exists && size) {
         if (size > r.unreadLen) {
            goto error;
         }

         if (!CPClipboard_SetItem(clip, fmt, r.pos, size)) {
            goto error;
         }
         if (!DnDSlideBuffer(&r, size)) {
            goto error;
         }
      }
   }

   /* It is possible that clip->changed is missing in some beta products. */
   if (r.unreadLen == sizeof clip->changed &&
       !DnDReadBuffer(&r, &clip->changed, sizeof clip->changed)) {
      goto error;
   }

   return TRUE;

error:
   CPClipboard_Destroy(clip);
   return FALSE;
}