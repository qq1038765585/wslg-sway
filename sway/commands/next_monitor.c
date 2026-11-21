#include <wlr/backend/interface.h>

#include "log.h"
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/server.h"

#include "sway/backend/wsland/wsland.h"

struct cmd_results *cmd_next_monitor(int argc, char **argv) {
	wlr_wsland_switch_next_monitor(server.backend);

	return cmd_default_border(argc, argv);
}
