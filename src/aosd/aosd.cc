/*
*
* Author: Giacomo Lozito <james@develia.org>, (C) 2005-2007
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* General Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with this program; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*
*/

#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>

#include "aosd.h"
#include "aosd_ui.h"
#include "aosd_osd.h"
#include "aosd_cfg.h"
#include "aosd_trigger.h"

static const char aosd_about[] =
 N_("Audacious OSD\n"
    "http://www.develia.org/projects.php?p=audacious#aosd\n\n"
    "Written by Giacomo Lozito <james@develia.org>\n\n"
    "Based in part on Evan Martin's Ghosd library:\n"
    "http://neugierig.org/software/ghosd/");

#define AUD_PLUGIN_NAME        N_("AOSD (On-Screen Display)")
#define AUD_PLUGIN_ABOUT       aosd_about
#define AUD_PLUGIN_INIT        aosd_init
#define AUD_PLUGIN_PREFS       & aosd_prefs
#define AUD_PLUGIN_CLEANUP     aosd_cleanup

#define AUD_DECLARE_GENERAL
#include <libaudcore/plugin-declare.h>

aosd_cfg_t * global_config = nullptr;
gboolean plugin_is_active = FALSE;


/* ***************** */
/* plug-in functions */

bool aosd_init (void)
{
  plugin_is_active = TRUE;
  g_log_set_handler( nullptr , G_LOG_LEVEL_WARNING , g_log_default_handler , nullptr );

  global_config = aosd_cfg_new();
  aosd_cfg_load( global_config );

  aosd_osd_init( global_config->osd->misc.transparency_mode );

  aosd_trigger_start( &global_config->osd->trigger );

  return TRUE;
}


void
aosd_cleanup ( void )
{
  if ( plugin_is_active == TRUE )
  {
    aosd_trigger_stop( &global_config->osd->trigger );

    aosd_osd_shutdown();
    aosd_osd_cleanup();

    if ( global_config != nullptr )
    {
      aosd_cfg_delete( global_config );
      global_config = nullptr;
    }

    plugin_is_active = FALSE;
  }
}
