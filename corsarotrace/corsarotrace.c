/*
 * corsaro
 *
 * Alistair King, CAIDA, UC San Diego
 * Shane Alcock, WAND, University of Waikato
 *
 * corsaro-info@caida.org
 *
 * Copyright (C) 2012 The Regents of the University of California.
 *
 * This file is part of corsaro.
 *
 * corsaro is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * corsaro is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with corsaro.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

#include <libtrace.h>
#include <libtrace_parallel.h>
#include <libtrace/message_queue.h>

#include "libcorsaro3_log.h"
#include "corsarotrace.h"
#include "libcorsaro3_plugin.h"
#include "libcorsaro3_filtering.h"

/* This version of corsaro is solely for the analysis of captured packets,
 * either via a live interface or from a trace file on disk.
 *
 * It is built on top of parallel libtrace, so we can have multiple
 * processing threads that each run the plugins against a subset of the
 * captured packets. The results from each thread+plugin will be written
 * to a temporary output file.
 *
 * We also have a single reporting thread which will merge the processing
 * results into a coherent output file for each plugin. This is where things
 * can get a little complicated -- we'll have to read and parse each of
 * the temporary output files but only once all processing threads have
 * finished writing to their respective file.
 */


libtrace_callback_set_t *processing = NULL;
libtrace_callback_set_t *reporter = NULL;

volatile int corsaro_halted = 0;
volatile int trace_halted = 0;

static void cleanup_signal(int sig) {
    (void)sig;
    corsaro_halted = 1;
    trace_halted = 1;
}


static void publish_file_closed_message(libtrace_t *trace,
        libtrace_thread_t *t, uint32_t last_interval, uint32_t rotatets) {

    corsaro_trace_msg_t *msg = NULL;
    libtrace_generic_t topub;

    msg = (corsaro_trace_msg_t *)malloc(sizeof(corsaro_trace_msg_t));

    msg->type = CORSARO_TRACE_MSG_ROTATE;
    msg->interval_num = last_interval;
    msg->interval_time = rotatets - 1;
    msg->plugindata = NULL;

    topub.ptr = msg;

    trace_publish_result(trace, t, ((uint64_t)rotatets) << 32, topub,
            RESULT_USER);

}

static void publish_interval_ended(libtrace_t *trace,
        libtrace_thread_t *t, uint32_t interval_num, uint32_t interval_ts,
        uint32_t endts, int plugincount, void **plugin_data) {

    corsaro_trace_msg_t *msg = NULL;
    libtrace_generic_t topub;

    msg = (corsaro_trace_msg_t *)malloc(sizeof(corsaro_trace_msg_t));

    msg->type = CORSARO_TRACE_MSG_MERGE;
    msg->interval_num = interval_num;
    msg->interval_time = interval_ts;
    msg->plugindata = plugin_data;

    topub.ptr = msg;

    trace_publish_result(trace, t, ((uint64_t)endts) << 32, topub,
            RESULT_USER);

}

static void publish_stop_message(libtrace_t *trace, libtrace_thread_t *t,
        uint64_t ts) {

    corsaro_trace_msg_t *msg = NULL;
    libtrace_generic_t topub;

    msg = (corsaro_trace_msg_t *)malloc(sizeof(corsaro_trace_msg_t));

    msg->type = CORSARO_TRACE_MSG_STOP;
    msg->interval_num = 0;
    msg->interval_time = 0;
    msg->plugindata = NULL;

    topub.ptr = msg;

    trace_publish_result(trace, t, ts, topub, RESULT_USER);
}

static inline int corsarotrace_interval_end(corsaro_logger_t *logger,
        libtrace_t *trace,
        libtrace_thread_t *t, corsaro_trace_local_t *tls, uint32_t ts) {
    void **interval_data;
    interval_data = corsaro_push_end_plugins(tls->plugins,
            tls->current_interval.number, ts);

    if (interval_data == NULL) {
        corsaro_log(logger,
                "error while pushing 'end interval' to plugins.");
        return -1;
    } else {
        publish_interval_ended(trace, t, tls->current_interval.number,
                tls->current_interval.time, ts,
                tls->plugins->plugincount, interval_data);
    }
    return 0;
}


static void *init_trace_processing(libtrace_t *trace, libtrace_thread_t *t,
        void *global) {

    corsaro_trace_global_t *glob = (corsaro_trace_global_t *)global;
    corsaro_trace_local_t *tls;


    if (glob->currenturi == 0) {
        tls = (corsaro_trace_local_t *)malloc(
                sizeof(corsaro_trace_local_t));

        tls->plugins = corsaro_start_plugins(glob->logger,
                glob->active_plugins, glob->plugincount,
                trace_get_perpkt_thread_id(t));

        tls->next_report = 0;
        tls->current_interval.number = 0;
        tls->current_interval.time = 0;
        tls->pkts_outstanding = 0;
        tls->pkts_since_tick = 0;
        tls->last_ts = 0;
        tls->stopped = 0;
        tls->customfilters = corsaro_create_filters(glob->logger,
                glob->treefiltername);

        if (glob->taggingon) {
            tls->tagger = corsaro_create_packet_tagger(glob->logger);
            if (tls->tagger == NULL) {
                corsaro_log(glob->logger,
                        "out of memory while creating packet tagger.");
            }

            if (glob->pfxtagopts.enabled) {
                if (corsaro_enable_ipmeta_provider(tls->tagger,
                        IPMETA_PROVIDER_PFX2AS, &(glob->pfxtagopts)) != 0) {
                    corsaro_log(glob->logger,
                            "error while enabling prefix->asn tagging.");
                }
            }
            if (glob->maxtagopts.enabled) {
                if (corsaro_enable_ipmeta_provider(tls->tagger,
                        IPMETA_PROVIDER_MAXMIND, &(glob->maxtagopts)) != 0) {
                    corsaro_log(glob->logger,
                            "error while enabling Maxmind geo-location tags.");
                }
            }
            if (glob->netacqtagopts.enabled) {
                if (corsaro_enable_ipmeta_provider(tls->tagger,
                        IPMETA_PROVIDER_NETACQ_EDGE,
                        &(glob->netacqtagopts)) != 0) {
                    corsaro_log(glob->logger,
                            "error while enabling Netacq-Edge geo-location tags.");
                }
            }
        } else {
            tls->tagger = NULL;
        }

        if (tls->plugins == NULL) {
            corsaro_log(glob->logger, "error while starting plugins.");
        }

    } else {
        tls = glob->savedlocalstate[trace_get_perpkt_thread_id(t)];
    }

    return tls;
}

static void halt_trace_processing(libtrace_t *trace, libtrace_thread_t *t,
        void *global, void *local) {
    corsaro_trace_global_t *glob = (corsaro_trace_global_t *)global;
    corsaro_trace_local_t *tls = (corsaro_trace_local_t *)local;

    if (glob->currenturi == glob->totaluris) {
        if (tls->pkts_outstanding) {
            if (corsarotrace_interval_end(glob->logger, trace, t, tls,
                        tls->last_ts) == -1) {
                /* do something?? */
            }
        }

        if (corsaro_stop_plugins(tls->plugins) == -1) {
            corsaro_log(glob->logger, "error while stopping plugins.");
        }

        if (!tls->stopped) {
            publish_file_closed_message(trace, t, tls->current_interval.number,
                            tls->next_report);
        }
        corsaro_destroy_filters(tls->customfilters);
        corsaro_destroy_packet_tagger(tls->tagger);
        corsaro_log(glob->logger, "shut down trace processing thread %d",
                trace_get_perpkt_thread_id(t));
        free(tls);
    } else {
        glob->savedlocalstate[trace_get_perpkt_thread_id(t)] = tls;
    }
}

static libtrace_packet_t *per_packet(libtrace_t *trace, libtrace_thread_t *t,
        void *global, void *local, libtrace_packet_t *packet) {

    corsaro_trace_global_t *glob = (corsaro_trace_global_t *)global;
    corsaro_trace_local_t *tls = (corsaro_trace_local_t *)local;
    struct timeval tv;
    corsaro_packet_tags_t packettags;

    if (tls->stopped) {
        return packet;
    }

    tv = trace_get_timeval(packet);

    if (glob->boundstartts && tv.tv_sec < glob->boundstartts) {
        return packet;
    }

    if (glob->boundendts && tv.tv_sec >= glob->boundendts) {
        if (corsarotrace_interval_end(glob->logger, trace, t, tls,
                    glob->boundendts) == -1) {
            /* do something?? */
        }
        publish_file_closed_message(trace, t, tls->current_interval.number,
                glob->boundendts);
        publish_stop_message(trace, t, ((uint64_t)glob->boundendts) << 32);
        tls->stopped = 1;
        tls->pkts_outstanding = 0;
        return packet;
    }


    if (tls->current_interval.time == 0) {
        /* First non-ignored packet */
        if (glob->interval <= 0) {
            corsaro_log(glob->logger,
                    "interval has somehow been assigned a bad value of %u\n",
                    glob->interval);
            exit(1);
        }

        tls->current_interval.time = tv.tv_sec - (tv.tv_sec % glob->interval);
        tls->lastrotateinterval.time = tls->current_interval.time;
        corsaro_push_start_plugins(tls->plugins, tls->current_interval.number,
                tls->current_interval.time);
        tls->next_report = tls->current_interval.time + glob->interval;
    }

    if (tv.tv_sec < tls->current_interval.time) {
        corsaro_log(glob->logger,
                "received a packet from *before* our current interval!");
        corsaro_log(glob->logger,
                "skipping packet, but this is probably a b00g.");
        return packet;
    }

    /* check if we have passed the end of an interval */
    while (tls->next_report && tv.tv_sec >= tls->next_report) {
        if (corsarotrace_interval_end(glob->logger, trace, t, tls,
                    tls->next_report) == -1) {
            /* do something?? */
        }
        if (glob->rotatefreq > 0 &&
                ((tls->current_interval.number + 1) % glob->rotatefreq) == 0) {
            publish_file_closed_message(trace, t, tls->current_interval.number,
                    tls->next_report);
        }
        tls->current_interval.number ++;
        tls->current_interval.time = tls->next_report;
        corsaro_push_start_plugins(tls->plugins, tls->current_interval.number,
            tls->current_interval.time);
        tls->next_report += glob->interval;
        tls->pkts_outstanding = 0;
    }

    if (glob->removespoofed &&
            corsaro_apply_spoofing_filter(glob->logger, packet)) {
        return packet;
    }

    if (glob->removeerratic && corsaro_apply_erratic_filter(glob->logger,
                packet)) {
        return packet;
    }

    if (glob->removerouted && corsaro_apply_routable_filter(glob->logger,
                packet)) {
        return packet;
    }

    tls->pkts_outstanding ++;
    tls->pkts_since_tick ++;
    tls->last_ts = tv.tv_sec;
    if (tls->tagger) {
        if (corsaro_tag_packet(tls->tagger, &packettags, packet) != 0) {
            corsaro_log(glob->logger,
                    "error while attempting to tag a packet.");
        }
        corsaro_push_packet_plugins(tls->plugins, packet, &packettags);
    } else {
        corsaro_push_packet_plugins(tls->plugins, packet, NULL);
    }

    return packet;
}

static void process_tick(libtrace_t *trace, libtrace_thread_t *t,
        void *global, void *local, uint64_t tick) {

    corsaro_trace_global_t *glob = (corsaro_trace_global_t *)global;
    corsaro_trace_local_t *tls = (corsaro_trace_local_t *)local;

    /* If we go an entire interval of real time without seeing a packet,
     * then the thread is probably not getting any packets. Let's force
     * an interval end here to try and keep things moving, otherwise
     * our merging thread is never going to do any work.
     *
     * NOTE: this is generally a bad state to be in -- if the ticks
     * are necessary, you really should be looking into another
     * hashing method that will distribute your packets more evenly
     * or reduce the number of threads that you're using to prevent
     * any from being idle.
     */

    if (tls->pkts_since_tick == 0) {

        if (corsarotrace_interval_end(glob->logger, trace, t, tls,
                    glob->boundendts) == -1) {
            /* do something?? */
        }
        if (glob->rotatefreq > 0 &&
                ((tls->current_interval.number + 1) % glob->rotatefreq) == 0) {
            publish_file_closed_message(trace, t, tls->current_interval.number,
                    tls->next_report);
        }
        tls->current_interval.number ++;
        tls->current_interval.time = tls->next_report;
        corsaro_push_start_plugins(tls->plugins, tls->current_interval.number,
            tls->current_interval.time);
        tls->next_report += glob->interval;
        tls->pkts_outstanding = 0;
        corsaro_log(glob->logger,
                "forced an interval to end within idle processing thread.");
    }

    tls->pkts_since_tick = 0;
}

static void *init_waiter(libtrace_t *trace, libtrace_thread_t *t,
        void *global) {

    corsaro_trace_global_t *glob = (corsaro_trace_global_t *)global;
    corsaro_trace_waiter_t *wait = (corsaro_trace_waiter_t *)malloc(
            sizeof(corsaro_trace_waiter_t));

    wait->stops_seen = 0;
    wait->finished_intervals = NULL;
    wait->next_rotate_interval = 0;
    wait->pluginset = corsaro_start_merging_plugins(glob->logger,
            glob->active_plugins, glob->plugincount, glob->threads);

    return wait;
}

static void halt_waiter(libtrace_t *trace, libtrace_thread_t *t,
        void *global, void *tls) {

    corsaro_trace_global_t *glob = (corsaro_trace_global_t *)global;
    corsaro_trace_waiter_t *wait = (corsaro_trace_waiter_t *)tls;

    corsaro_fin_interval_t *fin;

    while (wait->finished_intervals) {
        fin = wait->finished_intervals;

        corsaro_merge_plugin_outputs(glob->logger, wait->pluginset, fin);
        wait->finished_intervals = fin->next;
        free(fin);
    }

    corsaro_stop_plugins(wait->pluginset);

    free(wait);
    trace_halted = 1;
}

static void handle_trace_msg(libtrace_t *trace, libtrace_thread_t *t,
        void *global, void *tls, libtrace_result_t *result) {

    corsaro_trace_global_t *glob = (corsaro_trace_global_t *)global;
    corsaro_trace_waiter_t *wait = (corsaro_trace_waiter_t *)tls;
    corsaro_trace_msg_t *msg;

    if (result->type != RESULT_USER) {
        return;
    }

    msg = (corsaro_trace_msg_t *)result->value.ptr;

    if (msg->type == CORSARO_TRACE_MSG_STOP) {
        wait->stops_seen ++;
        if (wait->stops_seen == glob->threads) {
            trace_pstop(trace);
        }
    }

    if (msg->type == CORSARO_TRACE_MSG_ROTATE) {
        corsaro_fin_interval_t *fin = wait->finished_intervals;

        if (fin == NULL && wait->next_rotate_interval <= msg->interval_num) {
            corsaro_rotate_plugin_output(glob->logger, wait->pluginset);
            wait->next_rotate_interval = msg->interval_num + 1;
            free(msg);
            return;
        }

        while (fin != NULL) {
            if (fin->interval_id == msg->interval_num) {
                fin->rotate_after = 1;
                break;
            }
            fin = fin->next;
        }
        assert(fin != NULL);
    }

    if (msg->type == CORSARO_TRACE_MSG_MERGE) {
        corsaro_fin_interval_t *fin = wait->finished_intervals;
        corsaro_fin_interval_t *prev = NULL;

        if (glob->threads == 1) {
            corsaro_fin_interval_t quik;
            quik.interval_id = msg->interval_num;
            quik.timestamp = msg->interval_time;
            quik.threads_ended = 1;
            quik.next = NULL;
            quik.rotate_after = 0;
            quik.thread_plugin_data = (void ***)(calloc(glob->threads,
                sizeof(void **)));
            quik.thread_plugin_data[0] = msg->plugindata;

            corsaro_merge_plugin_outputs(glob->logger, wait->pluginset,
                    &quik);
            free(msg->plugindata);
            free(quik.thread_plugin_data);
            free(msg);
            return;
        }

        while (fin != NULL) {
            if (fin->interval_id == msg->interval_num) {
                break;
            }
            prev = fin;
            fin = fin->next;
        }

        if (fin != NULL) {
            int i;

            fin->thread_plugin_data[fin->threads_ended] = msg->plugindata;
            fin->threads_ended ++;
            if (fin->threads_ended == glob->threads) {
                assert(fin == wait->finished_intervals);
                corsaro_merge_plugin_outputs(glob->logger,
                        wait->pluginset, fin);
                if (fin->rotate_after) {
                    corsaro_rotate_plugin_output(glob->logger,
                            wait->pluginset);
                    wait->next_rotate_interval = msg->interval_num + 1;
                }
                wait->finished_intervals = fin->next;
                for (i = 0; i < glob->threads; i++) {
                    free(fin->thread_plugin_data[i]);
                }
                free(fin->thread_plugin_data);
                free(fin);
            }
        } else {
            fin = (corsaro_fin_interval_t *)malloc(
                    sizeof(corsaro_fin_interval_t));
            fin->interval_id = msg->interval_num;
            fin->timestamp = msg->interval_time;
            fin->threads_ended = 1;
            fin->next = NULL;
            fin->rotate_after = 0;
            fin->thread_plugin_data = (void ***)(calloc(glob->threads,
                    sizeof(void **)));
            fin->thread_plugin_data[0] = msg->plugindata;

            if (prev) {
                prev->next = fin;
            } else {
                wait->finished_intervals = fin;
            }
        }
    }
    free(msg);

}

int start_trace_input(corsaro_trace_global_t *glob) {

    libtrace_generic_t nothing;
    nothing.ptr = NULL;

    glob->trace = trace_create(glob->inputuris[glob->currenturi]);
    if (trace_is_err(glob->trace)) {
        libtrace_err_t err = trace_get_err(glob->trace);
        corsaro_log(glob->logger, "unable to create trace object: %s",
                err.problem);
        return -1;
    }

    trace_set_reporter_thold(glob->trace, 1);
    if (glob->interval > 0) {
        trace_set_tick_interval(glob->trace, glob->interval * 1000);
    }

    trace_set_combiner(glob->trace, &combiner_unordered, nothing);
    trace_set_hasher(glob->trace, HASHER_BIDIRECTIONAL, glob->hasher,
            glob->hasher_data);
    trace_set_perpkt_threads(glob->trace, glob->threads);

    if (glob->savedlocalstate == NULL) {
        glob->savedlocalstate = (corsaro_trace_local_t **)malloc(
                sizeof(corsaro_trace_local_t) * glob->threads);
    }

    if (!processing) {
        processing = trace_create_callback_set();
        trace_set_starting_cb(processing, init_trace_processing);
        trace_set_stopping_cb(processing, halt_trace_processing);
        trace_set_packet_cb(processing, per_packet);
        trace_set_tick_interval_cb(processing, process_tick);
    }

    if (!reporter) {
        reporter = trace_create_callback_set();
        trace_set_starting_cb(reporter, init_waiter);
        trace_set_stopping_cb(reporter, halt_waiter);
        trace_set_result_cb(reporter, handle_trace_msg);
    }

    if (glob->filterstring) {
        glob->filter = trace_create_filter(glob->filterstring);

        if (trace_set_filter(glob->trace, glob->filter) == -1)
        {
            libtrace_err_t err = trace_get_err(glob->trace);
            corsaro_log(glob->logger,
                    "unable to push filter to trace object: %s", err.problem);
            return -1;
        }
    }

    if (trace_pstart(glob->trace, glob, processing, reporter) == -1) {
        libtrace_err_t err = trace_get_err(glob->trace);
        corsaro_log(glob->logger, "unable to start reading from trace object: %s",
                err.problem);
        return -1;
    }

    corsaro_log(glob->logger, "successfully started input trace %s",
            glob->inputuris[glob->currenturi]);

    return 0;
}

void usage(char *prog) {
    printf("Usage: %s [ -l logmode ] -c configfile \n\n", prog);
    printf("Accepted logmodes:\n");
    printf("\tterminal\n\tfile\n\tsyslog\n\tdisabled\n");

}

int main(int argc, char *argv[]) {

    char *configfile = NULL;
    char *logmodestr = NULL;
    corsaro_trace_global_t *glob = NULL;
    int logmode = GLOBAL_LOGMODE_STDERR;

    struct sigaction sigact;
    sigset_t sig_before, sig_block_all;
    libtrace_stat_t *stats;

    /* Replaced old getopt-based nightmare with a proper YAML config file. */

    while (1) {
        int optind;
        struct option long_options[] = {
            { "help", 0, 0, 'h' },
            { "config", 1, 0, 'c'},
            { "log", 1, 0, 'l'},
            { NULL, 0, 0, 0 }
        };

        int c = getopt_long(argc, argv, "l:c:h", long_options,
                &optind);
        if (c == -1) {
            break;
        }

        switch(c) {
            case 'l':
                logmodestr = optarg;
                break;
            case 'c':
                configfile = optarg;
                break;
            case 'h':
                usage(argv[0]);
                return 1;
            default:
                fprintf(stderr, "corsarotrace: unsupported option: %c\n", c);
                usage(argv[0]);
                return 1;
        }
    }

    if (configfile == NULL) {
        fprintf(stderr, "corsarotrace: no config file specified. Use -c to specify one.\n");
        usage(argv[0]);
        return 1;
    }

    if (logmodestr != NULL) {
        if (strcmp(logmodestr, "stderr") == 0 ||
                    strcmp(logmodestr, "terminal") == 0) {
            logmode = GLOBAL_LOGMODE_STDERR;
        } else if (strcmp(logmodestr, "file") == 0) {
            logmode = GLOBAL_LOGMODE_FILE;
        } else if (strcmp(logmodestr, "syslog") == 0) {
            logmode = GLOBAL_LOGMODE_SYSLOG;
        } else if (strcmp(logmodestr, "disabled") == 0 ||
                strcmp(logmodestr, "off") == 0 ||
                strcmp(logmodestr, "none") == 0) {
            logmode = GLOBAL_LOGMODE_DISABLED;
        } else {
            fprintf(stderr, "corsarotrace: unexpected logmode: %s\n",
                    logmodestr);
            usage(argv[0]);
            return 1;
        }
    }

    sigact.sa_handler = cleanup_signal;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_RESTART;


    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    signal(SIGPIPE, SIG_IGN);

    glob = corsaro_trace_init_global(configfile, logmode);
    if (glob == NULL) {
        return 1;
    }

    while (glob->currenturi < glob->totaluris && !corsaro_halted) {

        sigemptyset(&sig_block_all);
        if (pthread_sigmask(SIG_SETMASK, &sig_block_all, &sig_before) < 0) {
            corsaro_log(glob->logger, "unable to disable signals before starting threads.");
            return 1;
        }
        trace_halted = 0;
        /* Create trace and start processing threads */
        if (start_trace_input(glob) < 0) {
            corsaro_log(glob->logger, "failed to start packet source %s.",
                    glob->inputuris[glob->currenturi]);
            glob->currenturi ++;
            trace_destroy(glob->trace);
            glob->trace = NULL;
            continue;
        }

        if (pthread_sigmask(SIG_SETMASK, &sig_before, NULL) < 0) {
            corsaro_log(glob->logger, "unable to re-enable signals after starting threads.");
            return 1;
        }

        while (!trace_halted) {
            sleep(1);
        }
        if (!trace_has_finished(glob->trace)) {
            glob->currenturi ++;
            trace_pstop(glob->trace);
        } else {
            glob->currenturi ++;
        }

        /* Join on input trace */
        trace_join(glob->trace);
        stats = trace_get_statistics(glob->trace, NULL);
        if (stats->dropped_valid) {
            corsaro_log(glob->logger, "dropped packet count: %lu",
                    stats->dropped);
        } else {
            corsaro_log(glob->logger, "dropped packet count: unknown");
        }

        if (stats->missing_valid) {
            corsaro_log(glob->logger, "missing packet count: %lu",
                    stats->missing);
        } else {
            corsaro_log(glob->logger, "missing packet count: unknown");
        }


        trace_destroy(glob->trace);
        glob->trace = NULL;
    }

    /* Join on merging thread */
    corsaro_log(glob->logger, "all threads have joined, exiting.");

    corsaro_trace_free_global(glob);

    if (processing) {
        trace_destroy_callback_set(processing);
    }
    if (reporter) {
        trace_destroy_callback_set(reporter);
    }

    return 0;
}

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
