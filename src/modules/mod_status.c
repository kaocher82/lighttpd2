/*
 * mod_status - display server status
 *
 * Description:
 *     mod_status can display a page with statistics like requests, traffic and active connections.
 *     It can be customized with different stylesheets (css)
 *
 * Setups:
 *     none
 * Options:
 *     status.css <name|url> - set the stylesheet to use, optional
 *         type: string; values: "default", "blue" or a url to an external css file
 * Actions:
 *     status.info           - returns the status info page to the client
 *
 * Example config:
 *     req.path == "/srv-status" {
 *         status.css = "http://mydomain/status.css";
 *         status.info;
 *     }
 *
 * Todo:
 *     - add querystring parameter format=plain
 *
 * Author:
 *     Copyright (c) 2008-2009 Thomas Porzelt
 * License:
 *     MIT, see COPYING file in the lighttpd 2 tree
 */

#include <lighttpd/base.h>
#include <lighttpd/collect.h>
#include <lighttpd/encoding.h>

LI_API gboolean mod_status_init(liModules *mods, liModule *mod);
LI_API gboolean mod_status_free(liModules *mods, liModule *mod);

static liHandlerResult status_info_runtime(liVRequest *vr, liPlugin *p);
static gint str_comp(gconstpointer a, gconstpointer b);

/* html snippet constants */
static const gchar header[] =
	"<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
	"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
	"         \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
	"<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
	"	<head>\n"
	"		<title>Lighttpd Status</title>\n";
static const gchar html_top[] =
	"		<div class=\"header\">Lighttpd Server Status | "
	"			<span style=\"font-size: 12px;\"><a href=\"?\">main</a></strong> - <a href=\"?mode=runtime\">runtime</a></span>"
	"		</div>\n"
	"		<div class=\"spacer\">\n"
	"			<strong>Hostname</strong>: <span>%s</span>"
	"			<strong>Uptime</strong>: <span>%s</span>\n"
	"			<strong>Started at</strong>: <span>%s</span>\n"
	"			<strong>Version</strong>: <span>" PACKAGE_VERSION " (" __DATE__ " " __TIME__ ")</span>\n"
	"		</div>\n";
static const gchar html_worker_th[] =
	"		<table cellspacing=\"0\">\n"
	"			<tr>\n"
	"				<th style=\"width: 100px;\"></th>\n"
	"				<th style=\"width: 175px;\">Requests</th>\n"
	"				<th style=\"width: 175px;\">Traffic in</th>\n"
	"				<th style=\"width: 175px;\">Traffic out</th>\n"
	"				<th style=\"width: 175px;\">Active connections</th>\n"
	"			</tr>\n";
static const gchar html_worker_th_avg[] =
	"		<table cellspacing=\"0\">\n"
	"			<tr>\n"
	"				<th style=\"width: 100px;\"></th>\n"
	"				<th style=\"width: 175px;\">Requests / s</th>\n"
	"				<th style=\"width: 175px;\">Traffic in / s</th>\n"
	"				<th style=\"width: 175px;\">Traffic out / s</th>\n"
	"				<th style=\"width: 175px;\">Active connections</th>\n"
	"			</tr>\n";
static const gchar html_worker_row[] =
	"			<tr class=\"%s\">\n"
	"				<td class=\"left\">%s</td>\n"
	"				<td>%s (%" G_GUINT64_FORMAT "%%)</td>\n"
	"				<td>%s (%" G_GUINT64_FORMAT "%%)</td>\n"
	"				<td>%s (%" G_GUINT64_FORMAT "%%)</td>\n"
	"				<td>%u (%u%%)</td>\n"
	"			</tr>\n";
static const gchar html_worker_row_avg[] =
	"			<tr class=\"%s\">\n"
	"				<td class=\"left\">%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%u</td>\n"
	"			</tr>\n";
static const gchar html_connections_th[] =
	"		<table cellspacing=\"0\">\n"
	"			<tr>\n"
	"				<th class=\"left\" style=\"width: 200px;\">Client</th>\n"
	"				<th style=\"width: 140px;\">State</th>\n"
	"				<th style=\"width: 170px;\">Host</th>\n"
	"				<th>Path+Querystring</th>\n"
	"				<th>Duration</th>\n"
	"				<th>Traffic in/out</th>\n"
	"				<th>Traffic in/out / s</th>\n"
	"				<th>Method</th>\n"
	"				<th>Request Size</th>\n"
	"				<th>Response Size</th>\n"
	"			</tr>\n";
static const gchar html_connections_row[] =
	"			<tr>\n"
	"				<td  class=\"left\">%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s</td>\n"
	"				<td class=\"left\">%s%s%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s / %s</td>\n"
	"				<td>%s / %s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s</td>\n"
	"				<td>%s</td>\n"
	"			</tr>\n";


static const gchar html_server_info[] =
	"		<table cellspacing=\"0\">\n"
	"			<tr>\n"
	"				<td  class=\"left\" style=\"width: 100px;\">Hostname</td>\n"
	"				<td class=\"left\">%s</td>\n"
	"			</tr>\n"
	"			<tr>\n"
	"				<td class=\"left\">User</td>\n"
	"				<td class=\"left\">%s (%u)</td>\n"
	"			</tr>\n"
	"				<td class=\"left\">Event handler</td>\n"
	"				<td class=\"left\">%s</td>\n"
	"		</table>\n";
static const gchar html_libinfo_th[] =
	"		<table cellspacing=\"0\">\n"
	"			<tr>\n"
	"				<th style=\"width: 100px;\"></th>\n"
	"				<th style=\"width: 100px;\">linked</th>\n"
	"				<th style=\"width: 100px;\">headers</th>\n"
	"			</tr>\n";
static const gchar html_libinfo_row[] =
	"			<tr>\n"
	"				<td class=\"left\">%s</td>\n"
	"				<td style=\"text-align: center;\">%s</td>\n"
	"				<td style=\"text-align: center;\">%s</td>\n"
	"			</tr>\n";
static const gchar html_libev_th[] =
	"		<table cellspacing=\"0\">\n"
	"			<tr>\n"
	"				<th style=\"width: 100px;\"></th>\n"
	"				<th style=\"width: 100px;\">select</th>\n"
	"				<th style=\"width: 100px;\">poll</th>\n"
	"				<th style=\"width: 100px;\">epoll</th>\n"
	"				<th style=\"width: 100px;\">kqueue</th>\n"
	"				<th style=\"width: 100px;\">/dev/poll</th>\n"
	"				<th style=\"width: 125px;\">solaris event port</th>\n"
	"			</tr>\n";
static const gchar html_libev_row[] =
	"			<tr>\n"
	"				<td class=\"left\">%s</td>\n"
	"				<td style=\"text-align: center;\">%s</td>\n"
	"				<td style=\"text-align: center;\">%s</td>\n"
	"				<td style=\"text-align: center;\">%s</td>\n"
	"				<td style=\"text-align: center;\">%s</td>\n"
	"				<td style=\"text-align: center;\">%s</td>\n"
	"				<td style=\"text-align: center;\">%s</td>\n"
	"			</tr>\n";


static const gchar css_default[] =
	"		<style type=\"text/css\">\n"
	"			body { margin: 0; padding: 0; font-family: \"lucida grande\",tahoma,verdana,arial,sans-serif; font-size: 12px; }\n"
	"			.header { padding: 5px; background-color: #6D84B4; font-size: 16px; color: white; border: 1px solid #3B5998; font-weight: bold; }\n"
	"			.header a { color: #F2F2F2; }\n"
	"			.spacer { background-color: #F2F2F2; border-bottom: 1px solid #CCC; padding: 5px; }\n"
	"			.spacer span { padding-right: 25px; }\n"
	"			.title { margin-left: 6px; margin-top: 20px; margin-bottom: 5px; }\n"
	"			.text { margin-left: 6px; margin-bottom: 5px; }\n"
	"			table { margin-left: 5px; border: 1px solid #CCC; }\n"
	"			th { font-weight: normal; padding: 3px; background-color: #E0E0E0;\n"
	"			border-bottom: 1px solid #BABABA; border-right: 1px solid #BABABA; border-top: 1px solid #FEFEFE; }\n"
	"			td { text-align: right; padding: 3px; border-bottom: 1px solid #F0F0F0; border-right: 1px solid #F8F8F8; }\n"
	"			.left { text-align: left; }\n"
	"			.totals td { border-top: 1px solid #DDDDDD; }\n"
	"		</style>\n";
/* blue theme by nitrox */
static const gchar css_blue[] =
	"		<style type=\"text/css\">\n"
	"			body { margin: 0; padding: 0; font-family: \"lucida grande\",tahoma,verdana,arial,sans-serif; font-size: 12px; background-color: #6d84b4; }\n"
	"			.header { padding: 5px; background-color: #6D84B4; font-size: 16px; color: white; border: 1px solid #3B5998; font-weight: bold; }\n"
	"			.header a { color: #F2F2F2; }\n"
	"			.spacer { background-color: #F2F2F2; border-bottom: 1px solid #CCC; padding: 5px; }\n"
	"			.spacer span { padding-right: 25px; }\n"
	"			.title { margin-left: 6px; margin-top: 20px; margin-bottom: 5px; }\n"
	"			.text { margin-left: 6px; margin-bottom: 5px; }\n"
	"			table { margin-left: 5px; border: 1px solid #CCC; }\n"
	"			th { font-weight: normal; padding: 3px; background-color: #E0E0E0;\n"
	"			border-bottom: 1px solid #BABABA; border-right: 1px solid #BABABA; border-top: 1px solid #FEFEFE; }\n"
	"			td { text-align: right; padding: 3px; border-bottom: 1px solid #F0F0F0; border-right: 1px solid #F8F8F8; }\n"
	"			.left { text-align: left; }\n"
	"			.totals td { border-top: 1px solid #DDDDDD; }\n"
	"		</style>\n";


struct mod_status_wrk_data;
typedef struct mod_status_wrk_data mod_status_wrk_data;

struct mod_status_con_data;
typedef struct mod_status_con_data mod_status_con_data;

struct mod_status_job;
typedef struct mod_status_job mod_status_job;

struct mod_status_con_data {
	guint worker_ndx;
	liConnectionState state;
	GString *remote_addr_str, *local_addr_str;
	gboolean is_ssl, keep_alive;
	GString *host, *path, *query;
	liHttpMethod method;
	goffset request_size;
	goffset response_size;
	ev_tstamp ts;
	guint64 bytes_in;
	guint64 bytes_out;
	guint64 bytes_in_5s_diff;
	guint64 bytes_out_5s_diff;
};

struct mod_status_wrk_data {
	guint worker_ndx;
	liStatistics stats;
	GArray *connections;
};

struct mod_status_job {
	liVRequest *vr;
	gpointer *context;
	liPlugin *p;
};


static gchar status_state_c(liConnectionState state) {
	static const gchar states[] = "dksrhw";
	return states[state];
}

/* the CollectFunc */
static gpointer status_collect_func(liWorker *wrk, gpointer fdata) {
	mod_status_wrk_data *sd = g_slice_new(mod_status_wrk_data);
	UNUSED(fdata);

	sd->stats = wrk->stats;
	sd->worker_ndx = wrk->ndx;
	/* gather connection info */
	sd->connections = g_array_sized_new(FALSE, TRUE, sizeof(mod_status_con_data), wrk->connections_active);
	g_array_set_size(sd->connections, wrk->connections_active);
	for (guint i = 0; i < wrk->connections_active; i++) {
		liConnection *c = g_array_index(wrk->connections, liConnection*, i);
		mod_status_con_data *cd = &g_array_index(sd->connections, mod_status_con_data, i);
		cd->is_ssl = c->is_ssl;
		cd->keep_alive = c->keep_alive;
		cd->remote_addr_str = g_string_new_len(GSTR_LEN(c->remote_addr_str));
		cd->local_addr_str = g_string_new_len(GSTR_LEN(c->srv_sock->local_addr_str));
		cd->host = g_string_new_len(GSTR_LEN(c->mainvr->request.uri.host));
		cd->path = g_string_new_len(GSTR_LEN(c->mainvr->request.uri.path));
		cd->query = g_string_new_len(GSTR_LEN(c->mainvr->request.uri.query));
		cd->method = c->mainvr->request.http_method;
		cd->request_size = c->mainvr->request.content_length;
		cd->response_size = c->mainvr->out->bytes_out;
		cd->state = c->state;
		cd->ts = c->ts;
		cd->bytes_in = c->stats.bytes_in;
		cd->bytes_out = c->stats.bytes_out;
		cd->bytes_in_5s_diff = c->stats.bytes_in_5s_diff;
		cd->bytes_out_5s_diff = c->stats.bytes_out_5s_diff;
	}
	return sd;
}

/* the CollectCallback */
static void status_collect_cb(gpointer cbdata, gpointer fdata, GPtrArray *result, gboolean complete) {
	mod_status_job *job = cbdata;
	liVRequest *vr;
	liPlugin *p;
	UNUSED(fdata);

	if (!complete) {
		/* someone called li_collect_break, so we don't need any vrequest handling here. just free the result data */
		guint i, j;

		for (i = 0; i < result->len; i++) {
			mod_status_wrk_data *sd = g_ptr_array_index(result, i);
			for (j = 0; j < sd->connections->len; j++) {
				mod_status_con_data *cd = &g_array_index(sd->connections, mod_status_con_data, j);

				g_string_free(cd->remote_addr_str, TRUE);
				g_string_free(cd->local_addr_str, TRUE);
				g_string_free(cd->host, TRUE);
				g_string_free(cd->path, TRUE);
			}

			g_array_free(sd->connections, TRUE);
			g_slice_free(mod_status_wrk_data, sd);
		}

		g_slice_free(mod_status_job, job);

		return;
	}

	vr = job->vr;
	p = job->p;
	/* clear context so it doesn't get cleaned up anymore */
	*(job->context) = NULL;
	g_slice_free(mod_status_job, job);

	if (complete) {
		GString *html;
		GString *tmpstr;
		GString *count_req, *count_bin, *count_bout;
		guint uptime;
		guint total_connections = 0;
		guint i;
		guint j;

		/* we got everything */
		liStatistics totals = {
			G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0),
			G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0),
			G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0), G_GUINT64_CONSTANT(0),
			0, 0, G_GUINT64_CONSTANT(0), 0, 0
		};

		uptime = CUR_TS(vr->wrk) - vr->wrk->srv->started;
		if (!uptime)
			uptime = 1;
		
		html = g_string_sized_new(8 * 1024 - 1);
		count_req = g_string_sized_new(10);
		count_bin = g_string_sized_new(10);
		count_bout = g_string_sized_new(10);
		tmpstr = g_string_sized_new(10);

		VR_DEBUG(vr, "finished collecting data: %s", complete ? "complete" : "not complete");

		/* calculate total stats over all workers */
		for (i = 0; i < result->len; i++) {
			mod_status_wrk_data *sd = g_ptr_array_index(result, i);

			totals.bytes_out += sd->stats.bytes_out;
			totals.bytes_in += sd->stats.bytes_in;
			totals.requests += sd->stats.requests;
			totals.actions_executed += sd->stats.actions_executed;
			total_connections += sd->connections->len;

			totals.requests_5s_diff += sd->stats.requests_5s_diff;
			totals.bytes_in_5s_diff += sd->stats.bytes_in_5s_diff;
			totals.bytes_out_5s_diff += sd->stats.bytes_out_5s_diff;
			totals.active_cons_cum += sd->stats.active_cons_cum;
			totals.active_cons_5s += sd->stats.active_cons_5s;
		}

		g_string_append_len(html, header, sizeof(header)-1);

		/* auto refresh */
		{
			gchar *val;
			guint len;
			gchar c;

			if (li_querystring_find(vr->request.uri.query, CONST_STR_LEN("refresh"), &val, &len)) {
				g_string_append_len(html, CONST_STR_LEN("<meta http-equiv=\"refresh\" content=\""));
				/* temp char swap */
				c = val[len]; val[len] = '\0';
				li_string_encode_append(val, html, LI_ENCODING_HTML);
				val[len] = c;
				g_string_append_len(html, CONST_STR_LEN("\">\n"));
			}
		}

		/* css */
		{
			GString *css = _OPTION(vr, p, 0).string;

			if (!css || !css->len) /* default css */
				g_string_append_len(html, css_default, sizeof(css_default)-1);
			else if (g_str_equal(css->str, "blue")) /* blue css */
				g_string_append_len(html, css_blue, sizeof(css_blue)-1);
			else /* external css */
				g_string_append_printf(html, "		<link rel=\"stylesheet\" rev=\"stylesheet\" href=\"%s\" media=\"screen\" />\n", css->str);
		}

		g_string_append_len(html, CONST_STR_LEN(
			"	</head>\n"
			"	<body>\n"
		));

		li_counter_format((guint64)(CUR_TS(vr->wrk) - vr->wrk->srv->started), COUNTER_TIME, tmpstr);
		g_string_append_printf(html, html_top,
			vr->request.uri.host->str,
			tmpstr->str,
			vr->wrk->srv->started_str->str
		);


		/* worker information, absolute values */
		{
			g_string_append_len(html, CONST_STR_LEN("		<div class=\"title\"><strong>Absolute stats</strong></div>\n"));

			g_string_append_len(html, html_worker_th, sizeof(html_worker_th)-1);

			#define PERCENTAGE(x, y) (y ? (x * 100 / y) : 0)
			for (i = 0; i < result->len; i++) {
				mod_status_wrk_data *sd = g_ptr_array_index(result, i);
				li_counter_format(sd->stats.requests, COUNTER_UNITS, count_req);
				li_counter_format(sd->stats.bytes_in, COUNTER_BYTES, count_bin);
				li_counter_format(sd->stats.bytes_out, COUNTER_BYTES, count_bout);
				g_string_printf(tmpstr, "Worker #%u", i+1);
				g_string_append_printf(html, html_worker_row, "", tmpstr->str,
					count_req->str, PERCENTAGE(sd->stats.requests, totals.requests),
					count_bin->str, PERCENTAGE(sd->stats.bytes_in, totals.bytes_in),
					count_bout->str, PERCENTAGE(sd->stats.bytes_out, totals.bytes_out),
					sd->connections->len, PERCENTAGE(sd->connections->len, total_connections));
			}
			#undef PERCENTAGE

			li_counter_format(totals.requests, COUNTER_UNITS, count_req);
			li_counter_format(totals.bytes_in, COUNTER_BYTES, count_bin);
			li_counter_format(totals.bytes_out, COUNTER_BYTES, count_bout);
			g_string_append_printf(html, html_worker_row, "totals", "Total",
				count_req->str, G_GUINT64_CONSTANT(100),
				count_bin->str, G_GUINT64_CONSTANT(100),
				count_bout->str, G_GUINT64_CONSTANT(100),
				total_connections, 100);
			g_string_append_len(html, CONST_STR_LEN("		</table>\n"));
		}

		/* worker information, avg values */
		{
			g_string_append_len(html, CONST_STR_LEN("<div class=\"title\"><strong>Average stats</strong> (since start)</div>\n"));

			g_string_append_len(html, html_worker_th_avg, sizeof(html_worker_th_avg)-1);

			#define PERCENTAGE(x) (sd->stat ## x ? (sd->stat ## x * 100 / total ## x) : 0)
			for (i = 0; i < result->len; i++) {
				mod_status_wrk_data *sd = g_ptr_array_index(result, i);

				li_counter_format(sd->stats.requests / uptime, COUNTER_UNITS, count_req);
				li_counter_format(sd->stats.bytes_in / uptime, COUNTER_BYTES, count_bin);
				li_counter_format(sd->stats.bytes_out / uptime, COUNTER_BYTES, count_bout);
				g_string_printf(tmpstr, "Worker #%u", i+1);
				g_string_append_printf(html, html_worker_row_avg, "", tmpstr->str,
					count_req->str,
					count_bin->str,
					count_bout->str,
					(guint)(sd->stats.active_cons_cum / uptime)
				);
			}
			#undef PERCENTAGE

			li_counter_format(totals.requests / uptime, COUNTER_UNITS, count_req);
			li_counter_format(totals.bytes_in / uptime, COUNTER_BYTES, count_bin);
			li_counter_format(totals.bytes_out / uptime, COUNTER_BYTES, count_bout);
			g_string_append_printf(html, html_worker_row_avg, "totals", "Total",
				count_req->str,
				count_bin->str,
				count_bout->str,
				(guint)(totals.active_cons_cum / uptime)
			);
			g_string_append_len(html, CONST_STR_LEN("		</table>\n"));
		}


		/* worker information, 5 seconds avg values */
		{
			g_string_append_len(html, CONST_STR_LEN("<div class=\"title\"><strong>Average stats</strong> (5 seconds)</div>\n"));

			g_string_append_len(html, html_worker_th_avg, sizeof(html_worker_th_avg)-1);

			#define PERCENTAGE(x) (sd->stat ## x ? (sd->stat ## x * 100 / total ## x) : 0)
			for (i = 0; i < result->len; i++) {
				mod_status_wrk_data *sd = g_ptr_array_index(result, i);

				li_counter_format(sd->stats.requests_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_UNITS, count_req);
				li_counter_format(sd->stats.bytes_in_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, count_bin);
				li_counter_format(sd->stats.bytes_out_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, count_bout);
				g_string_printf(tmpstr, "Worker #%u", i+1);
				g_string_append_printf(html, html_worker_row_avg, "", tmpstr->str,
					count_req->str,
					count_bin->str,
					count_bout->str,
					sd->stats.active_cons_5s
				);
			}
			#undef PERCENTAGE

			li_counter_format(totals.requests_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_UNITS, count_req);
			li_counter_format(totals.bytes_in_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, count_bin);
			li_counter_format(totals.bytes_out_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, count_bout);
			g_string_append_printf(html, html_worker_row_avg, "totals", "Total",
				count_req->str,
				count_bin->str,
				count_bout->str,
				totals.active_cons_5s
			);
			g_string_append_len(html, CONST_STR_LEN("		</table>\n"));
		}

		/* scoreboard */
		{
			guint k = 0;

			g_string_append_printf(html, "<div class=\"title\"><strong>%u connections</strong></div>\n<div class=\"text\">", total_connections);

			for (i = 0; i < result->len; i++) {
				mod_status_wrk_data *sd = g_ptr_array_index(result, i);
				for (j = 0; j < sd->connections->len; j++) {
					mod_status_con_data *cd = &g_array_index(sd->connections, mod_status_con_data, j);

					k++;

					if (k == 100) {
						g_string_append_len(html, CONST_STR_LEN("<br />\n"));
						k = 0;
					}

					g_string_append_c(html, status_state_c(cd->state));
				}
			}

			g_string_append_len(html, CONST_STR_LEN("</div>\n<div class=\"title\" style=\"margin-top: 10px;\"><strong>legend</strong></div>\n<div class=\"text\">"
				"d = dead, k = keep-alive, s = request start, r = read request header, h = handle main vrequest, w = write"
				"</div>\n"));
		}

		/* list connections */
		{
			GString *ts, *bytes_in, *bytes_out, *bytes_in_5s, *bytes_out_5s;
			GString *req_len, *resp_len;
			guint len;

			ts = g_string_sized_new(15);
			bytes_in = g_string_sized_new(10);
			bytes_out = g_string_sized_new(10);
			bytes_in_5s = g_string_sized_new(10);
			bytes_out_5s = g_string_sized_new(10);
			req_len = g_string_sized_new(10);
			resp_len = g_string_sized_new(10);

			g_string_append_len(html, CONST_STR_LEN("<div class=\"title\"><strong>Active connections</strong></div>\n"));
			g_string_append_len(html, html_connections_th, sizeof(html_connections_th)-1);
			for (i = 0; i < result->len; i++) {
				mod_status_wrk_data *sd = g_ptr_array_index(result, i);
				for (j = 0; j < sd->connections->len; j++) {
					mod_status_con_data *cd = &g_array_index(sd->connections, mod_status_con_data, j);

					li_counter_format((guint64)(CUR_TS(vr->wrk) - cd->ts), COUNTER_TIME, ts);
					li_counter_format(cd->bytes_in, COUNTER_BYTES, bytes_in);
					li_counter_format(cd->bytes_in_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, bytes_in_5s);
					li_counter_format(cd->bytes_out, COUNTER_BYTES, bytes_out);
					li_counter_format(cd->bytes_out_5s_diff / G_GUINT64_CONSTANT(5), COUNTER_BYTES, bytes_out_5s);
					li_counter_format(cd->request_size, COUNTER_BYTES, req_len);
					li_counter_format(cd->response_size, COUNTER_BYTES, resp_len);

					g_string_append_printf(html, html_connections_row,
						cd->remote_addr_str->str,
						li_connection_state_str(cd->state),
						cd->host->str,
						cd->path->str,
						cd->query->len ? "?":"",
						cd->query->len ? cd->query->str : "",
						ts->str,
						bytes_in->str,
						bytes_out->str,
						bytes_in_5s->str,
						bytes_out_5s->str,
						(cd->state >= LI_CON_STATE_HANDLE_MAINVR) ? li_http_method_string(cd->method, &len) : "",
						(cd->state >= LI_CON_STATE_HANDLE_MAINVR) ? req_len->str : "",
						(cd->state >= LI_CON_STATE_HANDLE_MAINVR) ? resp_len->str : ""
					);

					g_string_free(cd->remote_addr_str, TRUE);
					g_string_free(cd->local_addr_str, TRUE);
					g_string_free(cd->host, TRUE);
					g_string_free(cd->path, TRUE);
					g_string_free(cd->query, TRUE);
				}

				g_array_free(sd->connections, TRUE);
			}

			g_string_append_len(html, CONST_STR_LEN("		</table>\n"));

			g_string_free(ts, TRUE);
			g_string_free(bytes_in, TRUE);
			g_string_free(bytes_in_5s, TRUE);
			g_string_free(bytes_out, TRUE);
			g_string_free(bytes_out_5s, TRUE);
			g_string_free(req_len, TRUE);
			g_string_free(resp_len, TRUE);
		}

		/* free stats */
		for (i = 0; i < result->len; i++) {
			mod_status_wrk_data *sd = g_ptr_array_index(result, i);
			g_slice_free(mod_status_wrk_data, sd);
		}

		g_string_append_len(html, CONST_STR_LEN(
			" </body>\n"
			"</html>\n"
		));
		li_chunkqueue_append_string(vr->out, html);
		li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/html"));

		g_string_free(count_req, TRUE);
		g_string_free(count_bin, TRUE);
		g_string_free(count_bout, TRUE);
		g_string_free(tmpstr, TRUE);

		vr->response.http_status = 200;

		li_vrequest_joblist_append(vr);
	} else {
		/* something went wrong, client may have dropped the connection */
		VR_ERROR(vr, "%s", "collect request didn't end up complete");
		li_vrequest_error(vr);
	}
}

static liHandlerResult status_info(liVRequest *vr, gpointer param, gpointer *context) {
	if (li_vrequest_handle_direct(vr)) {
		gchar *val;
		guint len;

		if (!li_querystring_find(vr->request.uri.query, CONST_STR_LEN("mode"), &val, &len)) {
			/* no 'mode' query parameter given */
			liCollectInfo *ci;
			mod_status_job *j = g_slice_new(mod_status_job);
			j->vr = vr;
			j->context = context;
			j->p = (liPlugin*) param;

			VR_DEBUG(vr, "%s", "collecting stats...");

			ci = li_collect_start(vr->wrk, status_collect_func, NULL, status_collect_cb, j);
			*context = ci; /* may be NULL */
			return (*context) ? LI_HANDLER_WAIT_FOR_EVENT : LI_HANDLER_GO_ON;
		} else {
			/* 'mode' parameter given */
			if (strncmp(val, "runtime", len) == 0) {
				return status_info_runtime(vr, param);
			} else {
				VR_ERROR(vr, "%s", "status: unknown mode parameter");
				return LI_HANDLER_ERROR;
			}
		}
	}

	return LI_HANDLER_GO_ON;
}

static liHandlerResult status_info_cleanup(liVRequest *vr, gpointer param, gpointer context) {
	liCollectInfo *ci = (liCollectInfo*) context;

	UNUSED(vr);
	UNUSED(param);

	li_collect_break(ci);

	return LI_HANDLER_GO_ON;
}

static liAction* status_info_create(liServer *srv, liPlugin* p, liValue *val) {
	UNUSED(srv);
	UNUSED(val);

	return li_action_new_function(status_info, status_info_cleanup, NULL, p);
}

static gint str_comp(gconstpointer a, gconstpointer b) {
	return strcmp(*(const gchar**)a, *(const gchar**)b); 
}

static liHandlerResult status_info_runtime(liVRequest *vr, liPlugin *p) {
	GString *html;

	html = g_string_sized_new(2047);


	g_string_append_len(html, CONST_STR_LEN(header));

	/* auto refresh */
	{
		gchar *val;
		guint len;
		gchar c;

		if (li_querystring_find(vr->request.uri.query, CONST_STR_LEN("refresh"), &val, &len)) {
			g_string_append_len(html, CONST_STR_LEN("<meta http-equiv=\"refresh\" content=\""));
			/* temp char swap */
			c = val[len]; val[len] = '\0';
			li_string_encode_append(val, html, LI_ENCODING_HTML);
			val[len] = c;
			g_string_append_len(html, CONST_STR_LEN("\">\n"));
		}
	}

	/* css */
	{
		GString* css = _OPTION(vr, p, 0).string;

		if (!css || !css->len) /* default css */
			g_string_append_len(html, CONST_STR_LEN(css_default));
		else if (g_str_equal(css->str, "blue")) /* blue css */
			g_string_append_len(html, CONST_STR_LEN(css_blue));
		else /* external css */
			g_string_append_printf(html, "		<link rel=\"stylesheet\" rev=\"stylesheet\" href=\"%s\" media=\"screen\" />\n", css->str);
	}

	g_string_append_len(html, CONST_STR_LEN(
		"	</head>\n"
		"	<body>\n"
	));

	li_counter_format((guint64)(CUR_TS(vr->wrk) - vr->wrk->srv->started), COUNTER_TIME, vr->wrk->tmp_str);
	g_string_append_printf(html, html_top,
		vr->request.uri.host->str,
		vr->wrk->tmp_str->str,
		vr->wrk->srv->started_str->str
	);


	/* general info */
	{
		g_string_append_len(html, CONST_STR_LEN("		<div class=\"title\"><strong>Server info</strong></div>\n"));
		g_string_append_printf(html, html_server_info,
			g_get_host_name(), g_get_user_name(), getuid(), li_ev_backend_string(ev_backend(vr->wrk->loop))
		);
	}

	/* library info */
	{
		GString *tmp_str = g_string_sized_new(31);

		g_string_append_len(html, CONST_STR_LEN("		<div class=\"title\"><strong>Libraries</strong></div>\n"));
		g_string_append_len(html, CONST_STR_LEN(html_libinfo_th));

		g_string_truncate(tmp_str, 0);
		g_string_append_printf(tmp_str, "%u.%u.%u", glib_major_version, glib_minor_version, glib_micro_version);
		g_string_truncate(vr->wrk->tmp_str, 0);
		g_string_append_printf(vr->wrk->tmp_str, "%u.%u.%u", GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

		g_string_append_printf(html, html_libinfo_row, "GLib",
			tmp_str->str,
			vr->wrk->tmp_str->str
		);

		g_string_truncate(tmp_str, 0);
		g_string_append_printf(tmp_str, "%u.%u", ev_version_major(), ev_version_minor());
		g_string_truncate(vr->wrk->tmp_str, 0);
		g_string_append_printf(vr->wrk->tmp_str, "%u.%u", EV_VERSION_MAJOR, EV_VERSION_MINOR);

		g_string_append_printf(html, html_libinfo_row, "libev",
			tmp_str->str,
			vr->wrk->tmp_str->str
		);

		g_string_append_len(html, CONST_STR_LEN("		</table>\n"));
		g_string_free(tmp_str, TRUE);
	}

	/* libev info */
	{
		guint i;

		g_string_append_len(html, CONST_STR_LEN("		<div class=\"title\"><strong>libev backends</strong></div>\n"));
		g_string_append_len(html, CONST_STR_LEN(html_libev_th));

		/* supported backend, aka compiled in */
		i = ev_supported_backends();
		g_string_append_printf(html, html_libev_row, "supported",
			i & EVBACKEND_SELECT ? "yes" : "no",
			i & EVBACKEND_POLL ? "yes" : "no",
			i & EVBACKEND_EPOLL ? "yes" : "no",
			i & EVBACKEND_KQUEUE ? "yes" : "no",
			i & EVBACKEND_DEVPOLL ? "yes" : "no",
			i & EVBACKEND_PORT ? "yes" : "no"
		);

		/* recommended backends */
		i = ev_recommended_backends();
		g_string_append_printf(html, html_libev_row, "recommended",
			i & EVBACKEND_SELECT ? "yes" : "no",
			i & EVBACKEND_POLL ? "yes" : "no",
			i & EVBACKEND_EPOLL ? "yes" : "no",
			i & EVBACKEND_KQUEUE ? "yes" : "no",
			i & EVBACKEND_DEVPOLL ? "yes" : "no",
			i & EVBACKEND_PORT ? "yes" : "no"
		);

		g_string_append_len(html, CONST_STR_LEN("		</table>\n"));
	}

	/* list modules */
	{
		guint i, col;
		gpointer k, v;
		GHashTableIter iter;
		GArray *list = g_array_sized_new(FALSE, FALSE, sizeof(gchar*), g_hash_table_size(vr->wrk->srv->plugins));

		g_string_append_len(html, CONST_STR_LEN("		<div class=\"title\"><strong>Loaded modules</strong></div>\n"));
		g_string_append_len(html, CONST_STR_LEN("		<table cellspacing=\"0\">\n"));

		g_hash_table_iter_init(&iter, vr->wrk->srv->plugins);

		while (g_hash_table_iter_next(&iter, &k, &v))
			g_array_append_val(list, k);

		g_array_sort(list, str_comp);

		col = 0;

		for (i = 1; i < list->len; i++) {
			if (col == 0) {
				g_string_append_len(html, CONST_STR_LEN("			<tr>\n"));
				col++;
			}
			else if (col == 5) {
				g_string_append_len(html, CONST_STR_LEN("			</tr>\n"));
				col = 0;
				continue;
			} else {
				col++;
			}

			g_string_append_printf(html, "				<td class=\"left\">%s</td>\n", g_array_index(list, gchar*, i));
		}

		if (col) {
			for (i = 5 - col; i; i--)
				g_string_append_len(html, CONST_STR_LEN("				<td></td>\n"));

			g_string_append_len(html, CONST_STR_LEN("			</tr>\n"));
		}

		g_string_append_len(html, CONST_STR_LEN("		</table>\n"));

		g_array_free(list, TRUE);
	}

	/* list environment vars */
	{
		gchar **e;
		gchar **env = g_listenv();

		g_string_append_len(html, CONST_STR_LEN("		<div class=\"title\"><strong>Process environment</strong></div>\n"));
		g_string_append_len(html, CONST_STR_LEN("		<table cellspacing=\"0\">\n"));

		for (e = env; *e; e++) {
			g_string_append_printf(html, "			<tr><td class=\"left\">%s</td><td class=\"left\">%s</td></tr>\n", *e, getenv(*e));
		}

		g_string_append_len(html, CONST_STR_LEN("		</table>\n"));

		g_strfreev(env);
	}

	g_string_append_len(html, CONST_STR_LEN(
		" </body>\n"
		"</html>\n"
	));

	li_chunkqueue_append_string(vr->out, html);
	li_http_header_overwrite(vr->response.headers, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/html"));

	vr->response.http_status = 200;

	return LI_HANDLER_GO_ON;
}



static const liPluginOption options[] = {
	{ "status.css", LI_VALUE_STRING, NULL, NULL, NULL },

	{ NULL, 0, NULL, NULL, NULL }
};

static const liPluginAction actions[] = {
	{ "status.info", status_info_create },

	{ NULL, NULL }
};

static const liliPluginSetupCB setups[] = {
	{ NULL, NULL }
};


static void plugin_status_init(liServer *srv, liPlugin *p) {
	UNUSED(srv);

	p->options = options;
	p->actions = actions;
	p->setups = setups;
}


gboolean mod_status_init(liModules *mods, liModule *mod) {
	UNUSED(mod);

	MODULE_VERSION_CHECK(mods);

	mod->config = li_plugin_register(mods->main, "mod_status", plugin_status_init);

	return mod->config != NULL;
}

gboolean mod_status_free(liModules *mods, liModule *mod) {
	UNUSED(mods); UNUSED(mod);

	if (mod->config)
		li_plugin_free(mods->main, mod->config);

	return TRUE;
}
