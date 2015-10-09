/*
 * ngspice.c
 *
 * Authors:
 *  Ricardo Markiewicz <rmarkie@fi.uba.ar>
 *  Marc Lorber <lorber.marc@wanadoo.fr>
 *  Bernhard Schuster <bernhard@ahoi.io>
 *
 * Web page: https://ahoi.io/project/oregano
 *
 * Copyright (C) 1999-2001  Richard Hult
 * Copyright (C) 2003,2006  Ricardo Markiewicz
 * Copyright (C) 2009-2012  Marc Lorber
 * Copyright (C) 2014-2015  Bernhard Schuster
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <glib.h>
#include <glib/gprintf.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <glib/gi18n.h>

#include "ngspice.h"
#include "netlist-helper.h"
#include "echo.h"
#include "dialogs.h"
#include "engine-internal.h"
#include "ngspice-analysis.h"
#include "errors.h"

static void ngspice_class_init (NgSpiceClass *klass);
static void ngspice_finalize (GObject *object);
static void ngspice_dispose (GObject *object);
static void ngspice_instance_init (GTypeInstance *instance, gpointer g_class);
static void ngspice_interface_init (gpointer g_iface, gpointer iface_data);
static gboolean ngspice_child_stdout_cb (GIOChannel *source, GIOCondition condition,
                                         NgSpice *ngspice);
static gboolean ngspice_child_stderr_cb (GIOChannel *source, GIOCondition condition,
                                         NgSpice *ngspice);

static GObjectClass *parent_class = NULL;

G_DEFINE_TYPE_EXTENDED (NgSpice, ngspice, G_TYPE_OBJECT, 0, G_ADD_PRIVATE (NgSpice);
                        G_IMPLEMENT_INTERFACE (TYPE_ENGINE, ngspice_interface_init))

static void ngspice_class_init (NgSpiceClass *klass)
{
	GObjectClass *object_class;

	parent_class = g_type_class_peek_parent (klass);

	object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = ngspice_dispose;
	object_class->finalize = ngspice_finalize;
}

static void ngspice_finalize (GObject *object)
{
	NgSpice *ngspice = NGSPICE (object);
	GList *iter;
	int i;

	iter = ngspice->priv->analysis;
	for (; iter; iter = iter->next) {
		SimulationData *data = SIM_DATA (iter->data);
		g_free (data->var_names);
		g_free (data->var_units);
		for (i = 0; i < data->n_variables; i++)
			g_array_free (data->data[i], TRUE);

		g_free (data->min_data);
		g_free (data->max_data);

		g_free (data);
	}
	g_list_free (ngspice->priv->analysis);
	ngspice->priv->analysis = NULL;

	parent_class->finalize (object);
}

static void ngspice_dispose (GObject *object) { parent_class->dispose (object); }

static gboolean ngspice_has_warnings (Engine *self) { return FALSE; }

static gboolean ngspice_is_available (Engine *self)
{
	gchar *exe = g_find_program_in_path ("ngspice");

	if (!exe)
		return FALSE; // ngspice not found

	g_free (exe);
	return TRUE;
}

/**
 * \brief create a netlist buffer from the engine inernals
 *
 * @engine
 * @error [allow-none]
 */
static GString *ngspice_generate_netlist_buffer (Engine *engine, GError **error)
{
	GError *e = NULL;

	NgSpice *ngspice = NGSPICE (engine);
	Netlist output;
	GList *iter;

	GString *buffer = NULL;

	netlist_helper_create (ngspice->priv->schematic, &output, &e);
	if (e) {
		g_propagate_error (error, e);
		return NULL;
	}

	buffer = g_string_sized_new (500);
	if (!buffer) {
		g_set_error_literal (&e, OREGANO_ERROR, OREGANO_OUT_OF_MEMORY,
		                     "Failed to allocate intermediate buffer.");
		g_propagate_error (error, e);
		return NULL;
	}
	// Prints title
	g_string_append (buffer, "* ");
	g_string_append (buffer, output.title ? output.title : "Title: <unset>");
	g_string_append (buffer, "\n"
	                         "*----------------------------------------------"
	                         "\n"
	                         "*\tngspice - NETLIST"
	                         "\n");

	// Prints Options
	g_string_append (buffer, ".options OUT=120 ");

	iter = sim_settings_get_options (output.settings);
	for (; iter; iter = iter->next) {
		const SimOption *so = iter->data;
		// Prevent send NULL text
		if (so->value) {
			if (strlen (so->value) > 0) {
				g_string_append_printf (buffer, "%s=%s ", so->name, so->value);
			}
		}
	}
	g_string_append_c (buffer, '\n');

	// Include of subckt models
	g_string_append (buffer, "*------------- Models -------------------------\n");
	for (iter = output.models; iter; iter = iter->next) {
		const gchar *model = iter->data;
		g_string_append_printf (buffer, ".include %s/%s.model\n", OREGANO_MODELDIR, model);
	}

	// Prints template parts
	g_string_append (buffer, "*------------- Circuit Description-------------\n");
	g_string_append (buffer, output.template->str);
	g_string_append (buffer, "\n*----------------------------------------------\n");

	// Prints Transient Analysis
	if (sim_settings_get_trans (output.settings)) {
		gdouble st = 0;
		gdouble start = sim_settings_get_trans_start (output.settings);
		gdouble stop = sim_settings_get_trans_stop (output.settings);

		if (sim_settings_get_trans_step_enable (output.settings))
			st = sim_settings_get_trans_step (output.settings);
		else
			st = (stop - start) / 50;

		if ((stop - start) <= 0) {
			// FIXME ask for swapping or cancel simulation
			oregano_echo (_ ("Transient: Start time is after Stop time - fix this."
			                 "stop figure\n Swapping."));
			guint32 tmp = stop;
			stop = start;
			start = tmp;
		}

		g_string_append_printf (buffer, ".tran %lf %lf %lf\n", st, stop, start);
		{
			gchar *tmp_str = netlist_helper_create_analysis_string (output.store, FALSE);
			g_string_append_printf (buffer, ".print tran %s\n", tmp_str);
			g_free (tmp_str);
		}
		g_string_append_c (buffer, '\n');
	}

	//	Print DC Analysis
	if (sim_settings_get_dc (output.settings)) {
		g_string_append (buffer, ".dc ");
		if (sim_settings_get_dc_vsrc (output.settings)) {
			g_string_append_printf (buffer, "V_V%s %g %g %g\n",
			                        sim_settings_get_dc_vsrc (output.settings),
			                        sim_settings_get_dc_start (output.settings),
			                        sim_settings_get_dc_stop (output.settings),
			                        sim_settings_get_dc_step (output.settings));
			g_string_append_printf (buffer, ".print dc V(%s)\n",
			                        sim_settings_get_dc_vsrc (output.settings));
		}
	}

	// Prints AC Analysis
	if (sim_settings_get_ac (output.settings)) {
		g_string_append_printf (buffer, ".ac %s %d %g %g\n",
		                        sim_settings_get_ac_type (output.settings),
		                        sim_settings_get_ac_npoints (output.settings),
		                        sim_settings_get_ac_start (output.settings),
		                        sim_settings_get_ac_stop (output.settings));
	}

	// Prints analysis using a Fourier transform
	if (sim_settings_get_fourier (output.settings)) {
		g_string_append_printf (buffer, ".four %d %s\n",
		                        sim_settings_get_fourier_frequency (output.settings),
		                        sim_settings_get_fourier_nodes (output.settings));
	}

	g_string_append (buffer, ".op\n\n.END\n");

	return buffer;
}

/**
 * \brief generate a netlist and write to file
 *
 * @engine engine to extract schematic and settings from
 * @filename target netlist file, user selected
 * @error [allow-none]
 */
static gboolean ngspice_generate_netlist (Engine *engine, const gchar *filename, GError **error)
{
	GError *e = NULL;
	GString *buffer;
	gboolean success = FALSE;

	buffer = ngspice_generate_netlist_buffer (engine, &e);
	if (!buffer) {
		oregano_error ("Failed to generate netlist buffer\n");
		g_propagate_error (error, e);
		return FALSE;
	}

	success = g_file_set_contents (filename, buffer->str, buffer->len, &e);
	g_string_free (buffer, TRUE);

	if (!success) {
		g_propagate_error (error, e);
		oregano_error ("Failed to open file \"%s\" in 'w' mode.\n", filename);
		return FALSE;
	}
	return TRUE;
}

/**
 * this is total bogus
 */
static void ngspice_progress (Engine *self, double *d)
{
	NgSpice *ngspice = NGSPICE (self);

	ngspice->priv->progress += 0.1;
	if (ngspice->priv->progress > 1.)
		ngspice->priv->progress = 1.;
	(*d) = ngspice->priv->progress;
}

static void ngspice_stop (Engine *self)
{
	NgSpice *ngspice = NGSPICE (self);
	g_io_channel_shutdown (ngspice->priv->child_iochannel, TRUE, NULL);
	g_source_remove (ngspice->priv->child_iochannel_watch);
	g_spawn_close_pid (ngspice->priv->child_pid);
	g_spawn_close_pid (ngspice->priv->child_stdout);
}

/**
 * keeps an eye on the process itself
 */
static void ngspice_watch_cb (GPid pid, gint status, NgSpice *ngspice)
{
	// check for exit via return in main, exit() or _exit() of the child, see man
	// waitpid(2)
	if (WIFEXITED (status)) {
		gchar *line = NULL;
		gsize len;
		GError *e = NULL;
		g_io_channel_read_to_end (ngspice->priv->child_iochannel, &line, &len, &e);
		if (e) {
			schematic_log_append_error (ngspice->priv->schematic,
			                            "Failed to read from subprocess \"ngpsice\"");
			g_clear_error (&e);
			ngspice->priv->aborted = TRUE;
			g_signal_emit_by_name (G_OBJECT (ngspice), "aborted");
		} else if (len > 0) {
			fprintf (ngspice->priv->inputfp, "%s", line);
		}
		g_free (line);

		// Free stuff
		g_io_channel_shutdown (ngspice->priv->child_iochannel, TRUE, NULL);
		g_source_remove (ngspice->priv->child_iochannel_watch);
		g_spawn_close_pid (ngspice->priv->child_pid);
		g_spawn_close_pid (ngspice->priv->child_stdout);
		g_io_channel_shutdown (ngspice->priv->child_ioerror, TRUE, NULL);
		g_source_remove (ngspice->priv->child_ioerror_watch);
		g_spawn_close_pid (ngspice->priv->child_error);

		ngspice->priv->current = NULL;
		fclose (ngspice->priv->inputfp);
		ngspice_parse (ngspice);

		if (ngspice->priv->num_analysis == 0) {
			schematic_log_append_error (ngspice->priv->schematic,
			                            "### Too few or none analysis found ###\n");
			ngspice->priv->aborted = TRUE;
			g_signal_emit_by_name (G_OBJECT (ngspice), "aborted");
		} else {
			g_signal_emit_by_name (G_OBJECT (ngspice), "done");
		}
	}
}

static gboolean ngspice_child_stdout_cb (GIOChannel *source, GIOCondition condition,
                                         NgSpice *ngspice)
{
	gchar *line = NULL;
	gsize len, terminator;
	GError *e = NULL;

	g_io_channel_read_line (source, &line, &len, &terminator, &e);
	if (e) {
		schematic_log_append_error (ngspice->priv->schematic, "ngspice pipe stdout: %s - %i",
		                            e->message, e->code);
		g_clear_error (&e);
	} else if (len > 0) {
		// not an error ...
		fprintf (ngspice->priv->inputfp, "%s", line);
	}
	g_free (line);

	// Let UI update
	g_main_context_iteration (NULL, FALSE);
	return G_SOURCE_CONTINUE;
}

static gboolean ngspice_child_stderr_cb (GIOChannel *source, GIOCondition condition,
                                         NgSpice *ngspice)
{
	gchar *line;
	gsize len, terminator;
	GError *e = NULL;

	g_io_channel_read_line (source, &line, &len, &terminator, &e);
	if (e) {
		oregano_error ("ngspice pipe stderr: %s - %i", e->message, e->code);
		g_clear_error (&e);
	} else if (len > 0) {
		schematic_log_append_error (ngspice->priv->schematic, line);
	}
	g_free (line);

	// Let UI update
	g_main_context_iteration (NULL, FALSE);
	return G_SOURCE_CONTINUE;
}

static void ngspice_start (Engine *self)
{
	NgSpice *ngspice = NGSPICE (self);
	;
	NgSpicePrivate *priv = ngspice->priv;
	;
	GError *e = NULL;
	char *argv[] = {"ngspice", "-b", "/tmp/netlist.tmp", NULL};

	engine_generate_netlist (self, "/tmp/netlist.tmp", &e);
	if (e) {
		priv->aborted = TRUE;
		schematic_log_append_error (priv->schematic, e->message);
		g_signal_emit_by_name (G_OBJECT (ngspice), "aborted");
		g_clear_error (&e);
		return;
	}

	// Open the file storing the output of ngspice
	ngspice->priv->inputfp = fopen ("/tmp/netlist.lst", "w");

	if (g_spawn_async_with_pipes (NULL, // Working directory
	                              argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH, NULL,
	                              NULL, &priv->child_pid,
	                              NULL,                // STDIN
	                              &priv->child_stdout, // STDOUT
	                              &priv->child_error,  // STDERR
	                              &e)) {
		// Add a watch for process status
		g_child_watch_add_full (priv->child_pid, G_PRIORITY_HIGH_IDLE,
		                        (GChildWatchFunc)ngspice_watch_cb, ngspice, NULL);
		// Add a GIOChannel to read from process stdout
		priv->child_iochannel = g_io_channel_unix_new (priv->child_stdout);
		// Watch the I/O Channel to read child strout
		priv->child_iochannel_watch = g_io_add_watch_full (
		    priv->child_iochannel, G_PRIORITY_LOW, G_IO_IN | G_IO_PRI | G_IO_HUP | G_IO_NVAL,
		    (GIOFunc)ngspice_child_stdout_cb, ngspice, NULL);
		// Add a GIOChannel to read from process stderr
		priv->child_ioerror = g_io_channel_unix_new (priv->child_error);
		// Watch the I/O error channel to read the child sterr
		priv->child_ioerror_watch = g_io_add_watch_full (
		    priv->child_ioerror, G_PRIORITY_LOW, G_IO_IN | G_IO_PRI | G_IO_HUP | G_IO_NVAL,
		    (GIOFunc)ngspice_child_stderr_cb, ngspice, NULL);

	} else {
		priv->aborted = TRUE;
		schematic_log_append_error (priv->schematic, _ ("Unable to execute NgSpice."));
		g_signal_emit_by_name (G_OBJECT (ngspice), "aborted");
		g_clear_error (&e);
	}
}

static GList *ngspice_get_results (Engine *self)
{
	if (NGSPICE (self)->priv->analysis == NULL)
		oregano_error ("No frickin analysis there...");
	return NGSPICE (self)->priv->analysis;
}

static gchar *ngspice_get_operation (Engine *self)
{
	NgSpicePrivate *priv = NGSPICE (self)->priv;

	if (priv->current == NULL)
		return _ ("None");

	return engine_get_analysis_name (priv->current);
}

static void ngspice_interface_init (gpointer g_iface, gpointer iface_data)
{
	EngineInterface *klass = (EngineInterface *)g_iface;
	klass->start = ngspice_start;
	klass->stop = ngspice_stop;
	klass->progress = ngspice_progress;
	klass->get_netlist = ngspice_generate_netlist;
	klass->has_warnings = ngspice_has_warnings;
	klass->get_results = ngspice_get_results;
	klass->get_operation = ngspice_get_operation;
	klass->is_available = ngspice_is_available;
}

static void ngspice_init (GTypeInstance *instance)
{
	NgSpice *self = NGSPICE (instance);

	self->priv = ngspice_get_instance_private (self);
	self->priv->progress = 0.0;
	self->priv->char_last_newline = TRUE;
	self->priv->status = 0;
	self->priv->buf_count = 0;
	self->priv->num_analysis = 0;
	self->priv->analysis = NULL;
	self->priv->current = NULL;
	self->priv->aborted = FALSE;
}

Engine *ngspice_new (Schematic *sc)
{
	NgSpice *ngspice = NGSPICE (g_object_new (TYPE_NGSPICE, NULL));
	ngspice->priv->schematic = sc;

	return ENGINE (ngspice);
}
