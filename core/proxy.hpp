/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong
 *
 */

#pragma once

#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>
#include <unistd.h>

#include "global.hpp"
#include "coder.hpp"
#include "query.hpp"
#include "parser.hpp"
#include "planner.hpp"
#include "data_statistic.hpp"
#include "string_server.hpp"
#include "monitor.hpp"

#include "comm/adaptor.hpp"

// utils
#include "math.hpp"
#include "timer.hpp"

using namespace std;


// a vector of pointers of all local proxies
class Proxy;
std::vector<Proxy *> proxies;

class Proxy {

private:
    class Message {
    public:
        int sid;
        int tid;
        Bundle bundle;

        Message(int sid, int tid, Bundle &bundle)
            : sid(sid), tid(tid), bundle(bundle) { }
    };

    vector<Message> pending_msgs; // pending msgs to send

    // Collect candidate constants of all template types in given template query.
    // Result is in ptypes_grp of given template query.
    void fill_template(SPARQLQuery_Template &sqt) {
        sqt.ptypes_grp.resize(sqt.ptypes_str.size());
        for (int i = 0; i < sqt.ptypes_str.size(); i++) {
            string type = sqt.ptypes_str[i]; // the Types of random-constant

            // create a TYPE query to collect constants with the certain type
            SPARQLQuery request = SPARQLQuery();
            bool from_predicate = (type.find("fromPredicate") != string::npos);

            if (from_predicate) {
                // template types are defined by predicate
                // for example, %fromPredicate takeCourse ?X .
                // create a PREDICATE query to collect constants with the certain predicate
                int pos = sqt.ptypes_pos[i];
                ssid_t p = sqt.pattern_group.patterns[pos / 4].predicate;
                dir_t d;
                switch (pos % 4) {
                case 0:
                    d = (sqt.pattern_group.patterns[pos / 4].direction == OUT) ? IN : OUT;
                    break;
                case 3:
                    d = (sqt.pattern_group.patterns[pos / 4].direction == OUT) ? OUT : IN;
                    break;
                default:
                    ASSERT(false);
                }
                SPARQLQuery::Pattern pattern(p, PREDICATE_ID, d, -1);
                pattern.pred_type = 0;
                request.pattern_group.patterns.push_back(pattern);

                string dir_str = (d == OUT) ? "->" : "<-";
                type = "#Predicate [" + str_server->id2str[p] + " | " + dir_str + "]";
            } else {
                // templates are defined by type
                // for example, %GraduateStudent takeCourse ?X .
                // create a TYPE query to collect constants with the certain type
                SPARQLQuery::Pattern pattern(str_server->str2id[type], TYPE_ID, IN, -1);
                pattern.pred_type = 0;
                request.pattern_group.patterns.push_back(pattern);
            }

            request.result.nvars = 1;
            request.result.required_vars.push_back(-1);
            request.result.blind = false; // must take back the results

            setpid(request);
            send_request(request);

            SPARQLQuery reply = recv_reply();
            vector<sid_t> candidates(reply.result.result_table);

            // There is no candidate with the Type for a random-constant in the template
            // TODO: it should report empty for all queries of the template
            ASSERT(candidates.size() > 0);

            sqt.ptypes_grp[i] = candidates;

            logstream(LOG_INFO) << type << " has "
                                << sqt.ptypes_grp[i].size() << " candidates" << LOG_endl;
        }
    }

    // Send given bundle to given thread(@dst_tid) in given server(@dst_sid).
    // Return false if it fails. Bundle is pending in pending_msgs.
    inline bool send(Bundle &bundle, int dst_sid, int dst_tid) {
        if (adaptor->send(dst_sid, dst_tid, bundle))
            return true;

        pending_msgs.push_back(Message(dst_sid, dst_tid, bundle));
        return false;
    }

    // Send given bundle to certain engine in given server(@dst_sid).
    // Return false if it fails. Bundle is pending in pending_msgs.
    inline bool send(Bundle &bundle, int dst_sid) {
        // NOTE: the partitioned mapping has better tail latency in batch mode
        int range = global_num_engines / global_num_proxies;
        // FIXME: BUG if global_num_engines < global_num_proxies
        ASSERT(range > 0);

        int base = global_num_proxies + (range * tid);
        // randomly choose engine without preferred one
        int dst_eid = coder.get_random() % range;

        // If the preferred engine is busy, try the rest engines with round robin
        for (int i = 0; i < range; i++)
            if (adaptor->send(dst_sid, base + (dst_eid + i) % range, bundle))
                return true;

        pending_msgs.push_back(Message(dst_sid, (base + dst_eid), bundle));
        return false;
    }

    // Try send all msgs in pending_msgs.
    inline void sweep_msgs() {
        if (!pending_msgs.size()) return;

        logstream(LOG_DEBUG) << "#" << tid << " " << pending_msgs.size()
                             << " pending msgs on proxy." << LOG_endl;
        for (vector<Message>::iterator it = pending_msgs.begin();
                it != pending_msgs.end();) {
            if (adaptor->send(it->sid, it->tid, it->bundle))
                it = pending_msgs.erase(it);
            else
                ++it;
        }
    }

public:
    int sid;    // server id
    int tid;    // thread id

    String_Server *str_server;
    Adaptor *adaptor;

    Coder coder;
    Parser parser;
    Planner planner;
    data_statistic *statistic; // for planner


    Proxy(int sid, int tid, String_Server *str_server, DGraph * graph,
          Adaptor *adaptor, data_statistic *statistic)
        : sid(sid), tid(tid), str_server(str_server), adaptor(adaptor),
          coder(sid, tid), parser(str_server), statistic(statistic), planner(graph) { }

    void setpid(SPARQLQuery &r) { r.pid = coder.get_and_inc_qid(); }

    void setpid(RDFLoad &r) { r.pid = coder.get_and_inc_qid(); }

    void setpid(GStoreCheck &r) { r.pid = coder.get_and_inc_qid(); }

    // Send request to certain engine.
    void send_request(SPARQLQuery &r) {
        ASSERT(r.pid != -1);

        // submit the request to a certain server
        int start_sid = wukong::math::hash_mod(r.pattern_group.get_start(), global_num_servers);
        Bundle bundle(r);
#ifdef USE_GPU
        if (r.dev_type == SPARQLQuery::DeviceType::CPU) {
            logstream(LOG_DEBUG) << "dev_type is CPU, send to engine. r.pid=" << r.pid << LOG_endl;
            send(bundle, start_sid);
        } else if (r.dev_type == SPARQLQuery::DeviceType::GPU) {
            logstream(LOG_DEBUG) << "dev_type is GPU, send to GPU agent. r.pid=" << r.pid << LOG_endl;
            send(bundle, start_sid, WUKONG_GPU_AGENT_TID);
        } else {
            ASSERT_MSG(false, "Unknown device type");
        }
#else
        send(bundle, start_sid);

#endif  // end of USE_GPU
    }

    // Recv reply from engines.
    SPARQLQuery recv_reply(void) {
        Bundle bundle = adaptor->recv();
        ASSERT(bundle.type == SPARQL_QUERY);
        SPARQLQuery r = bundle.get_sparql_query();
        logstream(LOG_DEBUG) << "Proxy recv_reply: got reply id=" << r.id << ", r.pid=" << r.pid
                             << ", dev_type=" << (r.dev_type == SPARQLQuery::DeviceType::GPU ? "GPU" : "CPU")
                             << ", #rows=" << r.result.get_row_num() << ", step=" << r.pattern_step
                             << ", done: " << r.done(SPARQLQuery::SQState::SQ_PATTERN) << LOG_endl;
        return r;
    }

    // Try recv reply from engines.
    bool tryrecv_reply(SPARQLQuery &r) {
        Bundle bundle;
        bool success = adaptor->tryrecv(bundle);
        if (success) {
            ASSERT(bundle.type == SPARQL_QUERY);
            r = bundle.get_sparql_query();
        }

        return success;
    }

    // Run a single query for @cnt times. Command is "-f"
    // @is: input
    // @reply: result
    int run_single_query(istream &is, int mt_factor, int cnt,
                         istream &fmt_stream, int nopts, bool snd2gpu,
                         SPARQLQuery &reply, Monitor &monitor) {
        uint64_t start, end;
        SPARQLQuery request;

        // Parse the SPARQL query
        start = timer::get_usec();
        if (!parser.parse(is, request)) {
            logstream(LOG_ERROR) << "Parsing failed! ("
                                 << parser.strerror << ")" << LOG_endl;
            is.clear();
            is.seekg(0);
            return -2; // parsing failed
        }
        end = timer::get_usec();
        logstream(LOG_INFO) << "Parsing time: " << (end - start) << " usec" << LOG_endl;

        if (planner.set_query_plan(request.pattern_group, fmt_stream))
            logstream(LOG_INFO) << "User-defined query plan is enabled" << LOG_endl;
        else
            logstream(LOG_INFO) << "No query plan, turn on optimization" << LOG_endl;

        request.mt_factor = min(mt_factor, global_mt_threshold);

        // Generate query plan if SPARQL optimizer is enabled.
        // FIXME: currently, the optimizater only works for standard SPARQL query.
        if (global_enable_planner) {
            start = timer::get_usec();
            for (int i = 0; i < nopts; i ++) {
                planner.test = true;
                planner.generate_plan(request, statistic);
            }
            end = timer::get_usec();
            logstream(LOG_INFO) << "Optimization time: " << (end - start) / nopts << " usec" << LOG_endl;

            planner.test = false;
            // A shortcut for contradictory queries (e.g., empty result)
            if (planner.generate_plan(request, statistic) == false)
                return 0; // success, skip execution
        }

        // set the multi-threading factor for queries start from index
        if (request.start_from_index()) {
            if (mt_factor == 1 && global_mt_threshold > 1)
                logstream(LOG_EMPH) << "The query starts from an index vertex, "
                                    << "you could use option -m to accelerate it."
                                    << LOG_endl;
            // request.mt_factor = min(mt_factor, global_mt_threshold);
        }

        if (snd2gpu) {
#ifdef USE_GPU
            request.dev_type = SPARQLQuery::DeviceType::GPU;
            request.result.dev_type = SPARQLQuery::DeviceType::GPU;
            logstream(LOG_INFO) << "Leverage GPU to accelerate query processing." << LOG_endl;
#else
            snd2gpu = false;
            logstream(LOG_WARNING) << "Please build Wukong with GPU support "
                                   << "to turn on the \"-g\" option." << LOG_endl;
#endif
        }

        // Execute the SPARQL query
        monitor.init();
        for (int i = 0; i < cnt; i++) {
            setpid(request);
            // only take back results of the last request if not silent
            request.result.blind = i < (cnt - 1) ? true : global_silent;

            send_request(request);
            reply = recv_reply();
        }
        monitor.finish();

        return 0; // success
    } // end of run_single_query

    // Run a query emulator for @d seconds. Command is "-b"
    // Warm up for @w firstly, then measure throughput.
    // Latency is evaluated for @d seconds.
    // Proxy keeps @p queries in flight.
    int run_query_emu(istream &is, istream &fmt_stream, int d, int w, int p, Monitor &monitor) {
        uint64_t duration = SEC(d);
        uint64_t warmup = SEC(w);
        int parallel_factor = p;
        int try_rounds = 5; // rounds to try recv reply

        // parse the first line of batch config file
        // [#lights] [#heavies]
        int nlights, nheavies;
        is >> nlights >> nheavies;
        int ntypes = nlights + nheavies;

        if (ntypes <= 0 || nlights < 0 || nheavies < 0) {
            logstream(LOG_ERROR) << "Invalid #lights (" << nlights << " < 0)"
                                 << " or #heavies (" << nheavies << " < 0)!" << LOG_endl;
            return -2; // parsing failed
        }

        // read query-plan config file
        vector<string> fmt_file_names;
        if (fmt_stream.good()) {
            fmt_file_names.resize(ntypes);
            for (int i = 0; i < ntypes; i ++)
                fmt_stream >> fmt_file_names[i];
        }

        vector<SPARQLQuery_Template> tpls(nlights);
        vector<SPARQLQuery> heavy_reqs(nheavies);

        // parse template queries
        vector<int> loads(ntypes);
        for (int i = 0; i < ntypes; i++) {
            // each line is a class of light or heavy query
            // [fname] [#load]
            string fname;
            int load;

            is >> fname;
            ifstream ifs(fname);
            if (!ifs) {
                logstream(LOG_ERROR) << "Query file not found: " << fname << LOG_endl;
                return -1; // file not found
            }

            is >> load;
            ASSERT(load > 0);
            loads[i] = load;

            // parse the query
            bool success = i < nlights ?
                           parser.parse_template(ifs, tpls[i]) : // light query
                           parser.parse(ifs, heavy_reqs[i - nlights]); // heavy query

            if (!success) {
                logstream(LOG_ERROR) << "Template parsing failed!" << LOG_endl;
                return -2; // parsing failed
            }

            // generate a template for each class of light query
            if (i < nlights)
                fill_template(tpls[i]);

            // adapt user defined plan according to plan_config file
            if (!global_enable_planner && fmt_file_names.size() != 0) {
                ifstream fs(fmt_file_names[i]);
                if (!fs.good()) {
                    logstream(LOG_ERROR) << "Fail to read file: " << fmt_file_names[i] << LOG_endl;
                    return -2;
                }
                if (i < nlights) {
                    planner.set_query_plan(tpls[i].pattern_group, fs, tpls[i].ptypes_pos);
                }
                else {
                    planner.set_query_plan(heavy_reqs[i - nlights].pattern_group, fs);
                }
            }
        }

        monitor.init(ntypes);

        bool start = false; // start to measure throughput
        uint64_t send_cnt = 0, recv_cnt = 0, flying_cnt = 0;

        uint64_t init = timer::get_usec();
        // send requeries for duration seconds
        while ((timer::get_usec() - init) < duration) {
            // send requests
            for (int i = 0; i < parallel_factor - flying_cnt; i++) {
                sweep_msgs(); // sweep pending msgs first

                int idx = wukong::math::get_distribution(coder.get_random(), loads);
                SPARQLQuery request = idx < nlights ?
                                      tpls[idx].instantiate(coder.get_random()) : // light query
                                      heavy_reqs[idx - nlights]; // heavy query

                if (global_enable_planner) {
                    planner.generate_plan(request, statistic);
                }

                setpid(request);
                request.result.blind = true; // always not take back results for emulator

                monitor.start_record(request.pid, idx);
                send_request(request);

                send_cnt++;
            }

            // recieve replies (best of effort)
            for (int i = 0; i < try_rounds; i++) {
                SPARQLQuery r;
                while (tryrecv_reply(r)) {
                    recv_cnt++;
                    monitor.end_record(r.pid);
                }
            }

            monitor.print_timely_thpt(recv_cnt, sid, tid); // print throughput

            // start to measure throughput after first warmup seconds
            if (!start && (timer::get_usec() - init) > warmup) {
                monitor.start_thpt(recv_cnt);
                start = true;
            }

            flying_cnt = send_cnt - recv_cnt;
        }

        monitor.end_thpt(recv_cnt); // finish to measure throughput

        // recieve all replies to calculate the tail latency
        while (recv_cnt < send_cnt) {
            sweep_msgs();   // sweep pending msgs first

            SPARQLQuery r;
            while (tryrecv_reply(r)) {
                recv_cnt ++;
                monitor.end_record(r.pid);
            }

            monitor.print_timely_thpt(recv_cnt, sid, tid);
        }

        monitor.finish();

        return 0; // success
    } // end of run_query_emu

#ifdef DYNAMIC_GSTORE
    int dynamic_load_data(string &dname, RDFLoad &reply, Monitor &monitor, bool &check_dup) {
        monitor.init();

        RDFLoad request(dname, check_dup);
        setpid(request);
        for (int i = 0; i < global_num_servers; i++) {
            Bundle bundle(request);
            send(bundle, i);
        }

        int ret = 0;
        for (int i = 0; i < global_num_servers; i++) {
            Bundle bundle = adaptor->recv();
            ASSERT(bundle.type == DYNAMIC_LOAD);

            reply = bundle.get_rdf_load();
            if (reply.load_ret < 0)
                ret = reply.load_ret;
        }

        monitor.finish();
        return ret;
    }
#endif

    int gstore_check(GStoreCheck &reply, Monitor &monitor, bool i_enable, bool n_enable) {
        monitor.init();


        GStoreCheck request(i_enable, n_enable);
        setpid(request);
        for (int i = 0; i < global_num_servers; i++) {
            Bundle bundle(request);
            send(bundle, i);
        }

        int ret = 0;
        for (int i = 0; i < global_num_servers; i++) {
            Bundle bundle = adaptor->recv();
            ASSERT(bundle.type == GSTORE_CHECK);

            reply = bundle.get_gstore_check();
            if (reply.check_ret < 0)
                ret = reply.check_ret;
        }

        monitor.finish();
        return ret;

    }
};
