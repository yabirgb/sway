#define _POSIX_C_SOURCE 200809
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include "stringop.h"
#include "sway/input/input-manager.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "sway/output.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/node.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "list.h"
#include "log.h"
#include "util.h"

struct workspace_config *workspace_find_config(const char *ws_name) {
	for (int i = 0; i < config->workspace_configs->length; ++i) {
		struct workspace_config *wsc = config->workspace_configs->items[i];
		if (strcmp(wsc->workspace, ws_name) == 0) {
			return wsc;
		}
	}
	return NULL;
}

struct sway_output *workspace_get_initial_output(const char *name) {
	// Check workspace configs for a workspace<->output pair
	struct workspace_config *wsc = workspace_find_config(name);
	if (wsc) {
		for (int i = 0; i < wsc->outputs->length; i++) {
			struct sway_output *output =
				output_by_name_or_id(wsc->outputs->items[i]);
			if (output) {
				return output;
			}
		}
	}
	// Otherwise try to put it on the focused output
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_node *focus = seat_get_focus_inactive(seat, &root->node);
	if (focus && focus->type == N_WORKSPACE) {
		return focus->sway_workspace->output;
	} else if (focus && focus->type == N_CONTAINER) {
		return focus->sway_container->workspace->output;
	}
	// Fallback to the first output or noop output for headless
	return root->outputs->length ? root->outputs->items[0] : root->noop_output;
}

static void prevent_invalid_outer_gaps(struct sway_workspace *ws) {
	if (ws->gaps_outer.top < -ws->gaps_inner) {
		ws->gaps_outer.top = -ws->gaps_inner;
	}
	if (ws->gaps_outer.right < -ws->gaps_inner) {
		ws->gaps_outer.right = -ws->gaps_inner;
	}
	if (ws->gaps_outer.bottom < -ws->gaps_inner) {
		ws->gaps_outer.bottom = -ws->gaps_inner;
	}
	if (ws->gaps_outer.left < -ws->gaps_inner) {
		ws->gaps_outer.left = -ws->gaps_inner;
	}
}

struct sway_workspace *workspace_create(struct sway_output *output,
		const char *name) {
	if (output == NULL) {
		output = workspace_get_initial_output(name);
	}

	sway_log(SWAY_DEBUG, "Adding workspace %s for output %s", name,
			output->wlr_output->name);

	struct sway_workspace *ws = calloc(1, sizeof(struct sway_workspace));
	if (!ws) {
		sway_log(SWAY_ERROR, "Unable to allocate sway_workspace");
		return NULL;
	}
	node_init(&ws->node, N_WORKSPACE, ws);
	ws->name = name ? strdup(name) : NULL;
	ws->prev_split_layout = L_NONE;
	ws->layout = output_get_default_layout(output);
	ws->floating = create_list();
	ws->tiling = create_list();
	ws->output_priority = create_list();

	ws->gaps_outer = config->gaps_outer;
	ws->gaps_inner = config->gaps_inner;
	if (name) {
		struct workspace_config *wsc = workspace_find_config(name);
		if (wsc) {
			if (wsc->gaps_outer.top != INT_MIN) {
				ws->gaps_outer.top = wsc->gaps_outer.top;
			}
			if (wsc->gaps_outer.right != INT_MIN) {
				ws->gaps_outer.right = wsc->gaps_outer.right;
			}
			if (wsc->gaps_outer.bottom != INT_MIN) {
				ws->gaps_outer.bottom = wsc->gaps_outer.bottom;
			}
			if (wsc->gaps_outer.left != INT_MIN) {
				ws->gaps_outer.left = wsc->gaps_outer.left;
			}
			if (wsc->gaps_inner != INT_MIN) {
				ws->gaps_inner = wsc->gaps_inner;
			}
			// Since default outer gaps can be smaller than the negation of
			// workspace specific inner gaps, check outer gaps again
			prevent_invalid_outer_gaps(ws);

			// Add output priorities
			for (int i = 0; i < wsc->outputs->length; ++i) {
				char *name = wsc->outputs->items[i];
				if (strcmp(name, "*") != 0) {
					list_add(ws->output_priority, strdup(name));
				}
			}
		}
	}

	// If not already added, add the output to the lowest priority
	workspace_output_add_priority(ws, output);

	output_add_workspace(output, ws);
	output_sort_workspaces(output);

	ipc_event_workspace(NULL, ws, "init");
	wl_signal_emit(&root->events.new_node, &ws->node);

	return ws;
}

void workspace_destroy(struct sway_workspace *workspace) {
	if (!sway_assert(workspace->node.destroying,
				"Tried to free workspace which wasn't marked as destroying")) {
		return;
	}
	if (!sway_assert(workspace->node.ntxnrefs == 0, "Tried to free workspace "
				"which is still referenced by transactions")) {
		return;
	}

	free(workspace->name);
	free(workspace->representation);
	list_free_items_and_destroy(workspace->output_priority);
	list_free(workspace->floating);
	list_free(workspace->tiling);
	list_free(workspace->current.floating);
	list_free(workspace->current.tiling);
	free(workspace);
}

void workspace_begin_destroy(struct sway_workspace *workspace) {
	sway_log(SWAY_DEBUG, "Destroying workspace '%s'", workspace->name);
	ipc_event_workspace(NULL, workspace, "empty"); // intentional
	wl_signal_emit(&workspace->node.events.destroy, &workspace->node);

	if (workspace->output) {
		workspace_detach(workspace);
	}
	workspace->node.destroying = true;
	node_set_dirty(&workspace->node);
}

void workspace_consider_destroy(struct sway_workspace *ws) {
	if (ws->tiling->length || ws->floating->length) {
		return;
	}
	if (ws->output && output_get_active_workspace(ws->output) == ws) {
		return;
	}
	workspace_begin_destroy(ws);
}

static bool workspace_valid_on_output(const char *output_name,
		const char *ws_name) {
	struct workspace_config *wsc = workspace_find_config(ws_name);
	char identifier[128];
	struct sway_output *output = output_by_name_or_id(output_name);
	if (!output) {
		return false;
	}
	output_name = output->wlr_output->name;
	output_get_identifier(identifier, sizeof(identifier), output);

	if (!wsc) {
		return true;
	}

	for (int i = 0; i < wsc->outputs->length; i++) {
		if (strcmp(wsc->outputs->items[i], "*") == 0 ||
				strcmp(wsc->outputs->items[i], output_name) == 0 ||
				strcmp(wsc->outputs->items[i], identifier) == 0) {
			return true;
		}
	}

	return false;
}

static void workspace_name_from_binding(const struct sway_binding * binding,
		const char* output_name, int *min_order, char **earliest_name) {
	char *cmdlist = strdup(binding->command);
	char *dup = cmdlist;
	char *name = NULL;

	// workspace n
	char *cmd = argsep(&cmdlist, " ");
	if (cmdlist) {
		name = argsep(&cmdlist, ",;");
	}

	// TODO: support "move container to workspace" bindings as well

	if (strcmp("workspace", cmd) == 0 && name) {
		char *_target = strdup(name);
		_target = do_var_replacement(_target);
		strip_quotes(_target);
		sway_log(SWAY_DEBUG, "Got valid workspace command for target: '%s'",
				_target);

		// Make sure that the command references an actual workspace
		// not a command about workspaces
		if (strcmp(_target, "next") == 0 ||
				strcmp(_target, "prev") == 0 ||
				strncmp(_target, "next_on_output",
					strlen("next_on_output")) == 0 ||
				strncmp(_target, "prev_on_output",
					strlen("next_on_output")) == 0 ||
				strcmp(_target, "number") == 0 ||
				strcmp(_target, "back_and_forth") == 0 ||
				strcmp(_target, "current") == 0) {
			free(_target);
			free(dup);
			return;
		}

		// If the command is workspace number <name>, isolate the name
		if (strncmp(_target, "number ", strlen("number ")) == 0) {
			size_t length = strlen(_target) - strlen("number ") + 1;
			char *temp = malloc(length);
			strncpy(temp, _target + strlen("number "), length - 1);
			temp[length - 1] = '\0';
			free(_target);
			_target = temp;
			sway_log(SWAY_DEBUG, "Isolated name from workspace number: '%s'", _target);

			// Make sure the workspace number doesn't already exist
			if (isdigit(_target[0]) && workspace_by_number(_target)) {
				free(_target);
				free(dup);
				return;
			}
		}

		// Make sure that the workspace doesn't already exist
		if (workspace_by_name(_target)) {
			free(_target);
			free(dup);
			return;
		}

		// make sure that the workspace can appear on the given
		// output
		if (!workspace_valid_on_output(output_name, _target)) {
			free(_target);
			free(dup);
			return;
		}

		if (binding->order < *min_order) {
			*min_order = binding->order;
			free(*earliest_name);
			*earliest_name = _target;
			sway_log(SWAY_DEBUG, "Workspace: Found free name %s", _target);
		} else {
			free(_target);
		}
	}
	free(dup);
}

char *workspace_next_name(const char *output_name) {
	sway_log(SWAY_DEBUG, "Workspace: Generating new workspace name for output %s",
			output_name);
	// Scan for available workspace names by looking through output-workspace
	// assignments primarily, falling back to bindings and numbers.
	struct sway_mode *mode = config->current_mode;

	char identifier[128];
	struct sway_output *output = output_by_name_or_id(output_name);
	if (!output) {
		return NULL;
	}
	output_name = output->wlr_output->name;
	output_get_identifier(identifier, sizeof(identifier), output);

	int order = INT_MAX;
	char *target = NULL;
	for (int i = 0; i < mode->keysym_bindings->length; ++i) {
		workspace_name_from_binding(mode->keysym_bindings->items[i],
				output_name, &order, &target);
	}
	for (int i = 0; i < mode->keycode_bindings->length; ++i) {
		workspace_name_from_binding(mode->keycode_bindings->items[i],
				output_name, &order, &target);
	}
	for (int i = 0; i < config->workspace_configs->length; ++i) {
		// Unlike with bindings, this does not guarantee order
		const struct workspace_config *wsc = config->workspace_configs->items[i];
		if (workspace_by_name(wsc->workspace)) {
			continue;
		}
		bool found = false;
		for (int j = 0; j < wsc->outputs->length; ++j) {
			if (strcmp(wsc->outputs->items[j], "*") == 0 ||
					strcmp(wsc->outputs->items[j], output_name) == 0 ||
					strcmp(wsc->outputs->items[j], identifier) == 0) {
				found = true;
				free(target);
				target = strdup(wsc->workspace);
				break;
			}
		}
		if (found) {
			break;
		}
	}
	if (target != NULL) {
		return target;
	}
	// As a fall back, get the current number of active workspaces
	// and return that + 1 for the next workspace's name
	int ws_num = root->outputs->length;
	int l = snprintf(NULL, 0, "%d", ws_num);
	char *name = malloc(l + 1);
	if (!sway_assert(name, "Could not allocate workspace name")) {
		return NULL;
	}
	sprintf(name, "%d", ws_num++);
	return name;
}

static bool _workspace_by_number(struct sway_workspace *ws, void *data) {
	char *name = data;
	char *ws_name = ws->name;
	while (isdigit(*name)) {
		if (*name++ != *ws_name++) {
			return false;
		}
	}
	return !isdigit(*ws_name);
}

struct sway_workspace *workspace_by_number(const char* name) {
	return root_find_workspace(_workspace_by_number, (void *) name);
}

static bool _workspace_by_name(struct sway_workspace *ws, void *data) {
	return strcasecmp(ws->name, data) == 0;
}

struct sway_workspace *workspace_by_name(const char *name) {
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_workspace *current = seat_get_focused_workspace(seat);

	if (strcmp(name, "prev") == 0) {
		return workspace_prev(current);
	} else if (strcmp(name, "prev_on_output") == 0) {
		return workspace_output_prev(current, false);
	} else if (strcmp(name, "next") == 0) {
		return workspace_next(current);
	} else if (strcmp(name, "next_on_output") == 0) {
		return workspace_output_next(current, false);
	} else if (strcmp(name, "current") == 0) {
		return current;
	} else if (strcasecmp(name, "back_and_forth") == 0) {
		struct sway_seat *seat = input_manager_current_seat();
		if (!seat->prev_workspace_name) {
			return NULL;
		}
		return root_find_workspace(_workspace_by_name,
				(void*)seat->prev_workspace_name);
	} else {
		return root_find_workspace(_workspace_by_name, (void*)name);
	}
}

/**
 * Get the previous or next workspace on the specified output. Wraps around at
 * the end and beginning.  If next is false, the previous workspace is returned,
 * otherwise the next one is returned.
 */
static struct sway_workspace *workspace_output_prev_next_impl(
		struct sway_output *output, int dir, bool create) {
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_workspace *workspace = seat_get_focused_workspace(seat);

	int index = list_find(output->workspaces, workspace);
	if (!workspace_is_empty(workspace) && create &&
			(index + dir < 0 || index + dir == output->workspaces->length)) {
		struct sway_output *output = workspace->output;
		char *next = workspace_next_name(output->wlr_output->name);
		workspace_create(output, next);
		free(next);
	}
	size_t new_index = wrap(index + dir, output->workspaces->length);
	return output->workspaces->items[new_index];
}

/**
 * Get the previous or next workspace. If the first/last workspace on an output
 * is active, proceed to the previous/next output's previous/next workspace.
 */
static struct sway_workspace *workspace_prev_next_impl(
		struct sway_workspace *workspace, int dir) {
	struct sway_output *output = workspace->output;
	int index = list_find(output->workspaces, workspace);
	int new_index = index + dir;

	if (new_index >= 0 && new_index < output->workspaces->length) {
		return output->workspaces->items[new_index];
	}

	// Look on a different output
	int output_index = list_find(root->outputs, output);
	new_index = wrap(output_index + dir, root->outputs->length);
	output = root->outputs->items[new_index];

	if (dir == 1) {
		return output->workspaces->items[0];
	} else {
		return output->workspaces->items[output->workspaces->length - 1];
	}
}

struct sway_workspace *workspace_output_next(
		struct sway_workspace *current, bool create) {
	return workspace_output_prev_next_impl(current->output, 1, create);
}

struct sway_workspace *workspace_next(struct sway_workspace *current) {
	return workspace_prev_next_impl(current, 1);
}

struct sway_workspace *workspace_output_prev(
		struct sway_workspace *current, bool create) {
	return workspace_output_prev_next_impl(current->output, -1, create);
}

struct sway_workspace *workspace_prev(struct sway_workspace *current) {
	return workspace_prev_next_impl(current, -1);
}

bool workspace_switch(struct sway_workspace *workspace,
		bool no_auto_back_and_forth) {
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_workspace *active_ws = NULL;
	struct sway_node *focus = seat_get_focus_inactive(seat, &root->node);
	if (focus && focus->type == N_WORKSPACE) {
		active_ws = focus->sway_workspace;
	} else if (focus && focus->type == N_CONTAINER) {
		active_ws = focus->sway_container->workspace;
	}

	if (!no_auto_back_and_forth && config->auto_back_and_forth && active_ws
			&& active_ws == workspace && seat->prev_workspace_name) {
		struct sway_workspace *new_ws =
			workspace_by_name(seat->prev_workspace_name);
		workspace = new_ws ?
			new_ws :
			workspace_create(NULL, seat->prev_workspace_name);
	}

	if (active_ws && (!seat->prev_workspace_name ||
			(strcmp(seat->prev_workspace_name, active_ws->name)
				&& active_ws != workspace))) {
		free(seat->prev_workspace_name);
		seat->prev_workspace_name = malloc(strlen(active_ws->name) + 1);
		if (!seat->prev_workspace_name) {
			sway_log(SWAY_ERROR, "Unable to allocate previous workspace name");
			return false;
		}
		strcpy(seat->prev_workspace_name, active_ws->name);
	}

	sway_log(SWAY_DEBUG, "Switching to workspace %p:%s",
		workspace, workspace->name);
	struct sway_node *next = seat_get_focus_inactive(seat, &workspace->node);
	if (next == NULL) {
		next = &workspace->node;
	}
	seat_set_focus(seat, next);
	arrange_workspace(workspace);
	return true;
}

bool workspace_is_visible(struct sway_workspace *ws) {
	if (ws->node.destroying) {
		return false;
	}
	return output_get_active_workspace(ws->output) == ws;
}

bool workspace_is_empty(struct sway_workspace *ws) {
	if (ws->tiling->length) {
		return false;
	}
	// Sticky views are not considered to be part of this workspace
	for (int i = 0; i < ws->floating->length; ++i) {
		struct sway_container *floater = ws->floating->items[i];
		if (!floater->is_sticky) {
			return false;
		}
	}
	return true;
}

static int find_output(const void *id1, const void *id2) {
	return strcmp(id1, id2) ? 0 : 1;
}

void workspace_output_raise_priority(struct sway_workspace *ws,
		struct sway_output *old_output, struct sway_output *output) {
	int old_index = list_seq_find(ws->output_priority, find_output,
			old_output->wlr_output->name);
	if (old_index < 0) {
		return;
	}

	int new_index = list_seq_find(ws->output_priority, find_output,
			output->wlr_output->name);
	if (new_index < 0) {
		list_insert(ws->output_priority, old_index,
				strdup(output->wlr_output->name));
	} else if (new_index > old_index) {
		char *name = ws->output_priority->items[new_index];
		list_del(ws->output_priority, new_index);
		list_insert(ws->output_priority, old_index, name);
	}
}

void workspace_output_add_priority(struct sway_workspace *workspace,
		struct sway_output *output) {
	int index = list_seq_find(workspace->output_priority,
			find_output, output->wlr_output->name);
	if (index < 0) {
		list_add(workspace->output_priority, strdup(output->wlr_output->name));
	}
}

struct sway_output *workspace_output_get_highest_available(
		struct sway_workspace *ws, struct sway_output *exclude) {
	char exclude_id[128] = {'\0'};
	if (exclude) {
		output_get_identifier(exclude_id, sizeof(exclude_id), exclude);
	}

	for (int i = 0; i < ws->output_priority->length; i++) {
		char *name = ws->output_priority->items[i];
		if (exclude && (strcmp(name, exclude->wlr_output->name) == 0
					|| strcmp(name, exclude_id) == 0)) {
			continue;
		}

		struct sway_output *output = output_by_name_or_id(name);
		if (output) {
			return output;
		}
	}

	return NULL;
}

static bool find_urgent_iterator(struct sway_container *con, void *data) {
	return con->view && view_is_urgent(con->view);
}

void workspace_detect_urgent(struct sway_workspace *workspace) {
	bool new_urgent = (bool)workspace_find_container(workspace,
			find_urgent_iterator, NULL);

	if (workspace->urgent != new_urgent) {
		workspace->urgent = new_urgent;
		ipc_event_workspace(NULL, workspace, "urgent");
		output_damage_whole(workspace->output);
	}
}

void workspace_for_each_container(struct sway_workspace *ws,
		void (*f)(struct sway_container *con, void *data), void *data) {
	// Tiling
	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *container = ws->tiling->items[i];
		f(container, data);
		container_for_each_child(container, f, data);
	}
	// Floating
	for (int i = 0; i < ws->floating->length; ++i) {
		struct sway_container *container = ws->floating->items[i];
		f(container, data);
		container_for_each_child(container, f, data);
	}
}

struct sway_container *workspace_find_container(struct sway_workspace *ws,
		bool (*test)(struct sway_container *con, void *data), void *data) {
	struct sway_container *result = NULL;
	// Tiling
	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *child = ws->tiling->items[i];
		if (test(child, data)) {
			return child;
		}
		if ((result = container_find_child(child, test, data))) {
			return result;
		}
	}
	// Floating
	for (int i = 0; i < ws->floating->length; ++i) {
		struct sway_container *child = ws->floating->items[i];
		if (test(child, data)) {
			return child;
		}
		if ((result = container_find_child(child, test, data))) {
			return result;
		}
	}
	return NULL;
}

struct sway_container *workspace_wrap_children(struct sway_workspace *ws) {
	struct sway_container *fs = ws->fullscreen;
	struct sway_container *middle = container_create(NULL);
	middle->layout = ws->layout;
	while (ws->tiling->length) {
		struct sway_container *child = ws->tiling->items[0];
		container_detach(child);
		container_add_child(middle, child);
	}
	workspace_add_tiling(ws, middle);
	ws->fullscreen = fs;
	return middle;
}

void workspace_detach(struct sway_workspace *workspace) {
	struct sway_output *output = workspace->output;
	int index = list_find(output->workspaces, workspace);
	if (index != -1) {
		list_del(output->workspaces, index);
	}
	workspace->output = NULL;

	node_set_dirty(&workspace->node);
	node_set_dirty(&output->node);
}

static void set_workspace(struct sway_container *container, void *data) {
	container->workspace = container->parent->workspace;
}

void workspace_add_tiling(struct sway_workspace *workspace,
		struct sway_container *con) {
	if (con->workspace) {
		container_detach(con);
	}
	list_add(workspace->tiling, con);
	con->workspace = workspace;
	container_for_each_child(con, set_workspace, NULL);
	container_handle_fullscreen_reparent(con);
	workspace_update_representation(workspace);
	node_set_dirty(&workspace->node);
	node_set_dirty(&con->node);
}

void workspace_add_floating(struct sway_workspace *workspace,
		struct sway_container *con) {
	if (con->workspace) {
		container_detach(con);
	}
	list_add(workspace->floating, con);
	con->workspace = workspace;
	container_for_each_child(con, set_workspace, NULL);
	container_handle_fullscreen_reparent(con);
	node_set_dirty(&workspace->node);
	node_set_dirty(&con->node);
}

void workspace_insert_tiling(struct sway_workspace *workspace,
		struct sway_container *con, int index) {
	if (con->workspace) {
		container_detach(con);
	}
	list_insert(workspace->tiling, index, con);
	con->workspace = workspace;
	container_for_each_child(con, set_workspace, NULL);
	container_handle_fullscreen_reparent(con);
	workspace_update_representation(workspace);
	node_set_dirty(&workspace->node);
	node_set_dirty(&con->node);
}

void workspace_remove_gaps(struct sway_workspace *ws) {
	if (ws->current_gaps.top == 0 && ws->current_gaps.right == 0 &&
			ws->current_gaps.bottom == 0 && ws->current_gaps.left == 0) {
		return;
	}

	ws->width += ws->current_gaps.left + ws->current_gaps.right;
	ws->height += ws->current_gaps.top + ws->current_gaps.bottom;
	ws->x -= ws->current_gaps.left;
	ws->y -= ws->current_gaps.top;

	ws->current_gaps.top = 0;
	ws->current_gaps.right = 0;
	ws->current_gaps.bottom = 0;
	ws->current_gaps.left = 0;
}

void workspace_add_gaps(struct sway_workspace *ws) {
	if (ws->current_gaps.top > 0 || ws->current_gaps.right > 0 ||
			ws->current_gaps.bottom > 0 || ws->current_gaps.left > 0) {
		return;
	}
	if (config->smart_gaps) {
		struct sway_seat *seat = input_manager_get_default_seat();
		struct sway_container *focus =
			seat_get_focus_inactive_tiling(seat, ws);
		if (focus && !focus->view) {
			focus = seat_get_focus_inactive_view(seat, &focus->node);
		}
		if (focus && focus->view && view_is_only_visible(focus->view)) {
			return;
		}
	}

	ws->current_gaps = ws->gaps_outer;
	if (ws->layout == L_TABBED || ws->layout == L_STACKED) {
		// We have to add inner gaps for this, because children of tabbed and
		// stacked containers don't apply their own gaps - they assume the
		// tabbed/stacked container is using gaps.
		ws->current_gaps.top += ws->gaps_inner;
		ws->current_gaps.right += ws->gaps_inner;
		ws->current_gaps.bottom += ws->gaps_inner;
		ws->current_gaps.left += ws->gaps_inner;
	}

	ws->x += ws->current_gaps.left;
	ws->y += ws->current_gaps.top;
	ws->width -= ws->current_gaps.left + ws->current_gaps.right;
	ws->height -= ws->current_gaps.top + ws->current_gaps.bottom;
}

struct sway_container *workspace_split(struct sway_workspace *workspace,
		enum sway_container_layout layout) {
	if (workspace->tiling->length == 0) {
		workspace->prev_split_layout = workspace->layout;
		workspace->layout = layout;
		return NULL;
	}

	enum sway_container_layout old_layout = workspace->layout;
	struct sway_container *middle = workspace_wrap_children(workspace);
	workspace->layout = layout;
	middle->layout = old_layout;

	return middle;
}

void workspace_update_representation(struct sway_workspace *ws) {
	size_t len = container_build_representation(ws->layout, ws->tiling, NULL);
	free(ws->representation);
	ws->representation = calloc(len + 1, sizeof(char));
	if (!sway_assert(ws->representation, "Unable to allocate title string")) {
		return;
	}
	container_build_representation(ws->layout, ws->tiling, ws->representation);
}

void workspace_get_box(struct sway_workspace *workspace, struct wlr_box *box) {
	box->x = workspace->x;
	box->y = workspace->y;
	box->width = workspace->width;
	box->height = workspace->height;
}

static void count_tiling_views(struct sway_container *con, void *data) {
	if (con->view && !container_is_floating_or_child(con)) {
		size_t *count = data;
		*count += 1;
	}
}

size_t workspace_num_tiling_views(struct sway_workspace *ws) {
	size_t count = 0;
	workspace_for_each_container(ws, count_tiling_views, &count);
	return count;
}
