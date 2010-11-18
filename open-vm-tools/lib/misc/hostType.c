/*********************************************************
 * Copyright (C) 2006 VMware, Inc. All rights reserved.
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
 * hostType.c --
 *
 *    Platform-independent code that calls into hostType<OS>-specific
 *    code to determine the host OS type.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>

#include "vmware.h"
#include "hostType.h"
#include "str.h"

#if defined(linux)
#include <gnu/libc-version.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#if defined(VMX86_SERVER) || defined(VMX86_VPX)
#include "uwvmkAPI.h"
#endif // defined(VMX86_SERVER) || defined(VMX86_VPX)
#endif // defined(linux)

#define LGPFX "HOSTTYPE:"

/*
 *----------------------------------------------------------------------
 *
 * HostTypeOSVMKernelType --
 *
 *      Are we running on a flavor of VMKernel?  Only if the KERN_OSTYPE
 *      sysctl returns one of USERWORLD_SYSCTL_KERN_OSTYPE,
 *      USERWORLD_SYSCTL_VISOR_OSTYPE or USERWORLD_SYSCTL_VISOR64_OSTYPE
 *
 * Results:
 *      4 if running in a VMvisor UserWorld on the 64-bit vmkernel in ESX.
 *      3 if running directly in a UserWorld on the 64-bit vmkernel** in ESX.
 *      2 if running in a VMvisor UserWorld on the vmkernel in ESX.
 *      1 if running directly in a UserWorld on the vmkernel in ESX.
 *      0 if running on the COS or in a non-server product.
 *
 *      **Note that 64-bit vmkernel in ESX does not currently exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
HostTypeOSVMKernelType(void)
{
#if defined(linux)
   static int vmkernelType = -1;

   if (vmkernelType == -1) {
      if (strncmp(gnu_get_libc_release(), "vmware", 6) == 0) {
#if defined(VMX86_SERVER) || defined(VMX86_VPX)
         char osname[128];
         size_t osnameLength = sizeof(osname);
         int kernOsTypeCtl[] = { CTL_KERN, KERN_OSTYPE };

         if (sysctl(kernOsTypeCtl, ARRAYSIZE(kernOsTypeCtl),
                    osname, &osnameLength, 0, 0) == 0) {
            osnameLength = MAX(sizeof (osname), osnameLength);

            /*
             * XXX Yes, this is backwards in order of probability now, but we
             *     call it only once and anyway someday it won't be backwards ...
             */

            if (! strncmp(osname, USERWORLD_SYSCTL_VISOR64_OSTYPE,
                          osnameLength)) {
               vmkernelType = 4;
            } else if (! strncmp(osname, USERWORLD_SYSCTL_KERN64_OSTYPE,
                                 osnameLength)) {
               vmkernelType = 3;
            } else if (! strncmp(osname, USERWORLD_SYSCTL_VISOR_OSTYPE,
                                 osnameLength)) {
               vmkernelType = 2;
            } else if (! strncmp(osname, USERWORLD_SYSCTL_KERN_OSTYPE,
                                 osnameLength)) {
               vmkernelType = 1;
            } else {
               vmkernelType = 0;
            }
         } else {
            /*
             * XXX too many of the callers don't define Warning.  See bug 125455
             */

            vmkernelType = 0;
         }
#else // defined(VMX86_SERVER) || defined(VMX86_VPX)
         /*
          * Only binaries part of ESX and VPX are supposed to work in userworlds.
          * Hitting this assert means that you have built with incorrect PRODUCT.
          */

         NOT_REACHED();
#endif // defined(VMX86_SERVER) || defined(VMX86_VPX)
      } else {
         vmkernelType = 0;
      }
   }

   return (vmkernelType);
#else // defined(linux)
   /* Non-linux builds are never running on the VMKernel. */
   return 0;
#endif // defined(linux)
}


/*
 *----------------------------------------------------------------------
 *
 * HostType_OSIsVMK --
 *
 *      Are we running on the VMKernel (_any_ varient)?  True if KERN_OSTYPE
 *      sysctl returns _any_ of 
 *
 *          "UserWorld/VMKernel"
 *          "VMKernel"
 *          "UserWorld/VMKernel64"
 *          "VMKernel64"
 *
 * Results:
 *      TRUE if running in a UserWorld on the vmkernel in ESX.
 *      FALSE if running on the COS or in a non-server product.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
HostType_OSIsVMK(void)
{
   return (HostTypeOSVMKernelType() > 0);
}


/*
 *----------------------------------------------------------------------
 *
 * HostType_OSIsPureVMK --
 *
 *      Are we running on the VMvisor VMKernel (_any_ bitness)?  True if
 *      KERN_OSTYPE sysctl returns "VMKernel" or "VMKernel64".
 *
 * Results:
 *      TRUE if running in a VMvisor UserWorld on the vmkernel in ESX.
 *      FALSE if running on any other type of ESX or in a non-server product.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
HostType_OSIsPureVMK(void)
{
   return (HostTypeOSVMKernelType() == 2 || HostTypeOSVMKernelType() == 4);
}


/*
 *----------------------------------------------------------------------
 *
 * HostType_OSIsVMK64 --
 *
 *      Are we running on a 64-bit VMKernel?  Only if the KERN_OSTYPE
 *      sysctl returns "UserWorld/VMKernel64" or "VMKernel64".
 *
 * Results:
 *      TRUE if running in a UserWorld on a 64-bit vmkernel in ESX or VMvisor.
 *      FALSE if running on a 32-bit VMkernel in ESX or in a non-server product.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
HostType_OSIsVMK64(void)
{
   return (HostTypeOSVMKernelType() == 3 || HostTypeOSVMKernelType() == 4);
}
