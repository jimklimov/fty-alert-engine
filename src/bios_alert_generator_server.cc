/*  =========================================================================
    bios_alert_generator_server - Actor evaluating rules

    Copyright (C) 2014 - 2015 Eaton                                        
                                                                           
    This program is free software; you can redistribute it and/or modify   
    it under the terms of the GNU General Public License as published by   
    the Free Software Foundation; either version 2 of the License, or      
    (at your option) any later version.                                    
                                                                           
    This program is distributed in the hope that it will be useful,        
    but WITHOUT ANY WARRANTY; without even the implied warranty of         
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          
    GNU General Public License for more details.                           
                                                                           
    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.            
    =========================================================================
*/

/*
@header
    bios_alert_generator_server - Actor evaluating rules
@discuss
@end
*/
#include <string.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <fstream>
#include <malamute.h>
#include <bios_proto.h>
#include <math.h>
#include <functional>
#include <algorithm>

int agent_alert_verbose = 0;

#define zsys_debug1(...) \
    do { if (agent_alert_verbose) zsys_debug (__VA_ARGS__); } while (0);

#include "rule.h"
#include "normalrule.h"
#include "thresholdrulesimple.h"
#include "thresholdrulecomplex.h"
#include "regexrule.h"

#include "alertconfiguration.h"

//http://en.cppreference.com/w/cpp/language/typeid
//The header <typeinfo> must be included before using typeid
#include <typeinfo>


#define METRICS_STREAM "METRICS"
#define RULES_SUBJECT "rfc-evaluator-rules"

#include "../include/alert_agent.h"
#include "alert_agent_classes.h"

static void
list_rules(
    mlm_client_t *client,
    const char *type,
    AlertConfiguration &ac)
{
    std::function<bool(const std::string& s)> filter_f;

    if (streq (type,"all")) {
        filter_f = [](const std::string& s) {return true; };
    }
    else if (streq (type,"threshold")) {
        filter_f = [](const std::string& s) {return s.compare ("threshold") == 0; };
    }
    else if (streq (type,"single")) {
        filter_f = [](const std::string& s) {return s.compare ("single") == 0; };
    }
    else if (streq (type,"pattern")) {
        filter_f = [](const std::string& s) {return s.compare ("pattern") == 0; };
    }
    else {
        //invalid type
        zsys_warning ("type '%s' is invalid", type);
        zmsg_t *reply = zmsg_new ();
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "INVALID_TYPE");
        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    }
    zmsg_t *reply = zmsg_new ();
    zmsg_addstr (reply, "LIST");
    zmsg_addstr (reply, type);
    // std::vector <
    //  std::pair <
    //      RulePtr,
    //      std::vector<PureAlert>
    //      >
    // >
    zsys_debug1 ("number of all rules = '%zu'", ac.size ());
    for (const auto &i: ac) {
        const auto& rule = i.first;
        if (!filter_f(rule->whoami ())) {
            zsys_debug1 ("Skipping rule  = '%s'", rule->name().c_str());
            continue;
        }
        zsys_debug1 ("Adding rule  = '%s'", rule->name().c_str());
        zmsg_addstr (reply, rule->getJsonRule().c_str());
    }
    mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker(client), 1000, &reply);
}

static void
get_rule(
    mlm_client_t *client,
    const char *name,
    AlertConfiguration &ac)
{
    assert (name != NULL);
    zsys_debug1 ("number of all rules = '%zu'", ac.size ());
    for (const auto& i: ac) {
        const auto &rule = i.first;
        if (rule->hasSameNameAs (name))
        {
            zsys_debug1 ("found");
            zmsg_t *reply = zmsg_new ();
            zmsg_addstr (reply, "OK");
            zmsg_addstr (reply, rule->getJsonRule().c_str());

            mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker(client), 1000, &reply);
            return;
        }
    }

    zsys_debug1 ("not found");
    zmsg_t *reply = zmsg_new ();
    zmsg_addstr (reply, "ERROR");
    zmsg_addstr (reply, "NOT_FOUND");

    mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
    return;
}


static std::string
makeActionList(
    const std::vector <std::string> &actions)
{
    std::string s;
    bool first = true;
    for (const auto& oneAction : actions) {
        if (first) {
            s.append (oneAction);
            first = false;
        }
        else {
            s.append ("/").append (oneAction);
        }
    }
    return s;
}

static void
send_alerts(
    mlm_client_t *client,
    const std::vector <PureAlert> &alertsToSend,
    const std::string &rule_name)
{
    for ( const auto &alert : alertsToSend )
    {
        zmsg_t *msg = bios_proto_encode_alert (
            NULL,
            rule_name.c_str(),
            alert._element.c_str(),
            alert._status.c_str(),
            alert._severity.c_str(),
            alert._description.c_str(),
            ::time (NULL),
            makeActionList(alert._actions).c_str()
        );
        if( msg ) {
            std::string atopic = rule_name + "/"
                + alert._severity + "@"
                + alert._element;
            mlm_client_send (client, atopic.c_str(), &msg);
            zsys_debug1 ("mlm_client_send (subject = '%s')", atopic.c_str());
        }
    }
}

static void
send_alerts(
    mlm_client_t *client,
    const std::vector <PureAlert> &alertsToSend,
    const RulePtr &rule)
{
    send_alerts (client, alertsToSend, rule->name());
}

static void
add_rule(
    mlm_client_t *client,
    const char *json_representation,
    AlertConfiguration &ac)
{
    std::istringstream f(json_representation);
    std::set <std::string> newSubjectsToSubscribe;
    std::vector <PureAlert> alertsToSend;
    AlertConfiguration::iterator new_rule_it;
    int rv = ac.addRule (f, newSubjectsToSubscribe, alertsToSend, new_rule_it);
    zmsg_t *reply = zmsg_new ();
    switch (rv) {
    case -2:
    {
        // rule exists
        zsys_debug1 ("rule already exists");
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "ALREADY_EXISTS");

        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    }
    case 0:
    {
        // rule was created succesfully
        /* TODO: WIP, don't delete
        zsys_debug1 ("newsubjects count = %d", newSubjectsToSubscribe.size() );
        zsys_debug1 ("alertsToSend count = %d", alertsToSend.size() );
        for ( const auto &interestedSubject : newSubjectsToSubscribe ) {
            zsys_debug1 ("Registering to receive '%s'", interestedSubject.c_str());
            mlm_client_set_consumer(client, METRICS_STREAM, interestedSubject.c_str());
            zsys_debug1("Registering finished");
        }
        */

        // send a reply back
        zsys_debug1 ("rule added correctly");
        zmsg_addstr (reply, "OK");
        zmsg_addstr (reply, json_representation)
            ;
        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);

        // send updated alert
        send_alerts (client, alertsToSend, new_rule_it->first);
        return;
    }
    case -5:
    {
        zsys_debug1 ("rule has bad lua");
        // error during the rule creation (lua)
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "BAD_LUA");

        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    }
    case -6:
    {
        zsys_debug1 ("internal error");
        // error during the rule creation (lua)
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "Internal error - operating with storage/disk failed.");

        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    }
    default:
        // error during the rule creation
        zsys_debug1 ("default bad json");
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "BAD_JSON");

        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    }
}


static void
update_rule(
    mlm_client_t *client,
    const char *json_representation,
    const char *rule_name,
    AlertConfiguration &ac)
{
    std::istringstream f(json_representation);
    std::set <std::string> newSubjectsToSubscribe;
    std::vector <PureAlert> alertsToSend;
    AlertConfiguration::iterator new_rule_it;
    int rv = ac.updateRule (f, rule_name, newSubjectsToSubscribe, alertsToSend, new_rule_it);
    zmsg_t *reply = zmsg_new ();
    switch (rv) {
    case -2:
        zsys_debug1 ("rule not found");
        // ERROR rule doesn't exist
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "NOT_FOUND");
        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    case 0:
        // rule was updated succesfully
        /* TODO: WIP, don't delete
        zsys_debug1 ("newsubjects count = %d", newSubjectsToSubscribe.size() );
        zsys_debug1 ("alertsToSend count = %d", alertsToSend.size() );
        for ( const auto &interestedSubject : newSubjectsToSubscribe ) {
            zsys_debug1 ("Registering to receive '%s'", interestedSubject.c_str());
            mlm_client_set_consumer(client, METRICS_STREAM, interestedSubject.c_str());
            zsys_debug1("Registering finished");
        }
        */
        // send a reply back
        zsys_debug1 ("rule updated");
        zmsg_addstr (reply, "OK");
        zmsg_addstr (reply, json_representation);
        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        // send updated alert
        send_alerts (client, alertsToSend, new_rule_it->first);
        return;
    case -5:
        zsys_debug1 ("rule has incorrect lua");
        // error during the rule creation (lua)
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "BAD_LUA");
        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    case -3:
        zsys_debug1 ("new rule najme already exists");
        // rule with new rule name already exists
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "ALREADY_EXISTS");
        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    case -6:
    {
        // error during the rule creation
        zsys_debug1 ("internal error");
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "Internal error - operating with storate/disk failed.");
        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    }
    default:
        // error during the rule creation
        zsys_debug1 ("bad json default");
        zmsg_addstr (reply, "ERROR");
        zmsg_addstr (reply, "BAD_JSON");
        mlm_client_sendto (client, mlm_client_sender(client), RULES_SUBJECT, mlm_client_tracker (client), 1000, &reply);
        return;
    }
}

static void
evaluate_metric(
    mlm_client_t *client,
    const MetricInfo &triggeringMetric,
    const MetricList &knownMetricValues,
    AlertConfiguration &ac)
{
    // Go through all known rules, and try to evaluate them
    for ( const auto &i : ac ) {
        const auto &rule = i.first;
        try {
            bool is_interresting = rule->isTopicInteresting (triggeringMetric.generateTopic());

            if ( !is_interresting) {
                continue;
            }
            zsys_debug1 (" ### Evaluate rule '%s'", rule->name().c_str());

            PureAlert pureAlert;
            int rv = rule->evaluate (knownMetricValues, pureAlert);
            if ( rv != 0 ) {
                zsys_error (" ### Cannot evaluate the rule '%s'", rule->name().c_str());
                continue;
            }

            PureAlert alertToSend;
            rv = ac.updateAlert (rule, pureAlert, alertToSend);
            if ( rv == -1 ) {
                zsys_debug1 (" ### alert updated, nothing to send");
                // nothing to send
                continue;
            }
            send_alerts (client, {alertToSend}, rule);
        }
        catch ( const std::exception &e) {
            zsys_error ("CANNOT evaluate rule, because '%s'", e.what());
        }
    }
}

static void
s_statistics (
        uint64_t now,
        uint64_t previous,
        std::vector <uint64_t>& stream_messages,
        std::vector <uint64_t>& mailbox_messages) {
    double stream_mean = 0.0;
    double mailbox_mean = 0.0;

    uint64_t stream_median = 0;
    uint64_t mailbox_median = 0;

    if (!stream_messages.empty ()) {
        uint64_t sum = 0;
        for (std::size_t i = 0; i < stream_messages.size (); i++) {
	        sum += stream_messages.at (i);
	    }
    	stream_mean = sum / stream_messages.size ();

        std::nth_element (stream_messages.begin(), stream_messages.begin() + stream_messages.size()/2, stream_messages.end());
        stream_median = stream_messages[stream_messages.size ()/2];
    }

    if (!mailbox_messages.empty ()) {
        uint64_t sum = 0;
        for (std::size_t i = 0; i < mailbox_messages.size (); i++) {
            sum += mailbox_messages.at (i);
        }
        mailbox_mean = sum / mailbox_messages.size ();

        std::nth_element (mailbox_messages.begin(), mailbox_messages.begin() + mailbox_messages.size()/2, mailbox_messages.end());
        mailbox_median = mailbox_messages[mailbox_messages.size ()/2];
    }


    zsys_warning ("========== TIMING INFO ==========");
    zsys_warning (
            "\tTime period '%" PRIu64"' miliseconds\t(start: '%" PRIu64"', stop:'%" PRIu64"')", 
            now - previous,
            previous,
            now);
    zsys_warning (                            
            "\tNumber of messages received '%" PRIu64"', out of which",
            stream_messages.size () + mailbox_messages.size ());
    zsys_warning (
            "\t\tSTREAM  == '%" PRIu64"' messages. Mean time: '%s', median time: '%s'",
            stream_messages.size (),
            std::to_string (stream_mean).c_str (),
            std::to_string (stream_median).c_str ());
    zsys_warning (
            "\t\tMAILBOX == '%" PRIu64"' messages. Mean time: '%s', median time: '%s'",
            mailbox_messages.size (),
            std::to_string (mailbox_mean).c_str (),
            std::to_string (mailbox_median).c_str ());

    stream_messages.clear ();
    mailbox_messages.clear ();
} 

void
bios_alert_generator_server (zsock_t *pipe, void* args)
{
    MetricList cache; // need to track incoming measurements
    AlertConfiguration alertConfiguration;
    char *name = (char*) args;

    mlm_client_t *client = mlm_client_new ();
    assert (client);

    zpoller_t *poller = zpoller_new (pipe, mlm_client_msgpipe (client), NULL);
    assert (poller);

    uint64_t timestamp_stat = static_cast<uint64_t> (zclock_mono ());
    uint64_t timeout = 30000;

    std::vector <uint64_t> stream_messages;
    std::vector <uint64_t> mailbox_messages;


    zsock_signal (pipe, 0);

    while (!zsys_interrupted) {
        /* this function is broken under high load, comment it for now
        if ( needCheck && !mlm_client_connected (client) ) {
            zsys_error ("BIOS-2076: mlm client was disconnected, sacrifice the alert agent to be revived by systemd");
            break;
        }
        */
        void *which = zpoller_wait (poller, timeout);
        if (which == NULL) {
            if (zpoller_terminated (poller) || zsys_interrupted) {
                zsys_warning ("zpoller_terminated () or zsys_interrupted. Shutting down.");
                break;
            }
            if (zpoller_expired (poller)) {
                s_statistics (timestamp_stat + timeout, timestamp_stat, stream_messages, mailbox_messages);
            }
            timestamp_stat = static_cast<uint64_t> (zclock_mono ());
            continue;
        }

        uint64_t now = static_cast<uint64_t> (zclock_mono ());
        if (now - timestamp_stat >= timeout) {
            s_statistics (now, timestamp_stat, stream_messages, mailbox_messages);
            timestamp_stat = static_cast<uint64_t> (zclock_mono ());
        }

        if (which == pipe) {
            zsys_debug1 ("which == pipe");
            zmsg_t *msg = zmsg_recv (pipe);
            char *cmd = zmsg_popstr (msg);

            if (streq (cmd, "$TERM")) {
                zsys_debug1 ("$TERM received");
                zstr_free (&cmd);
                zmsg_destroy (&msg);
                goto exit;
            }
            else
            if (streq (cmd, "VERBOSE")) {
                zsys_debug1 ("VERBOSE received");
                agent_alert_verbose = true;
                zmsg_destroy (&msg);
            }
            else
            if (streq (cmd, "CONNECT")) {
                zsys_debug1 ("CONNECT received");
                char* endpoint = zmsg_popstr (msg);
                int rv = mlm_client_connect (client, endpoint, 1000, name);
                if (rv == -1)
                    zsys_error ("%s: can't connect to malamute endpoint '%s'", name, endpoint);
                zstr_free (&endpoint);
//                needCheck = true;
            }
            else
            if (streq (cmd, "PRODUCER")) {
                zsys_debug1 ("PRODUCER received");
                char* stream = zmsg_popstr (msg);
                int rv = mlm_client_set_producer (client, stream);
                if (rv == -1)
                    zsys_error ("%s: can't set producer on stream '%s'", name, stream);
                zstr_free (&stream);
            }
            else
            if (streq (cmd, "CONSUMER")) {
                zsys_debug1 ("CONSUMER received");
                char* stream = zmsg_popstr (msg);
                char* pattern = zmsg_popstr (msg);
                int rv = mlm_client_set_consumer (client, stream, pattern);
                if (rv == -1)
                    zsys_error ("%s: can't set consumer on stream '%s', '%s'", name, stream, pattern);
                zstr_free (&pattern);
                zstr_free (&stream);
            }
            else
            if (streq (cmd, "CONFIG")) {
                zsys_debug1 ("CONFIG received");
                char* filename = zmsg_popstr (msg);

                // Read initial configuration
                alertConfiguration.setPath(filename);
                // XXX: somes to subscribe are returned, but not used for now
                alertConfiguration.readConfiguration();

                zstr_free (&filename);
            }
            zstr_free (&cmd);
            zmsg_destroy (&msg);
            continue;
        }
        zsys_debug1 ("which != pipe");

        // This agent is a reactive agent, it reacts only on messages
        // and doesn't do anything if there is no messages
        // TODO: probably alert also should be send every XXX seconds,
        // even if no measurements were recieved
        zmsg_t *zmessage = mlm_client_recv (client);
        if ( zmessage == NULL ) {
            continue;
        }
        std::string topic = mlm_client_subject(client);
        // There are two possible inputs and they come in different ways
        // from the stream  -> metrics
        // from the mailbox -> rules
        //                  -> request for rule list
        // but even so we try to decide according what we got, not from where
        if( is_bios_proto(zmessage) ) {
            bios_proto_t *bmessage = bios_proto_decode(&zmessage);
            if( ! bmessage ) {
                zsys_error ("cannot decode message with topic %s, ignoring", topic.c_str());
                continue;
            }
            if ( bios_proto_id(bmessage) == BIOS_PROTO_METRIC )  {
                // process as metric message
                const char *type = bios_proto_type(bmessage);
                const char *element_src = bios_proto_element_src(bmessage);
                const char *value = bios_proto_value(bmessage);
                const char *unit = bios_proto_unit(bmessage);
                uint32_t ttl = bios_proto_ttl(bmessage);
                uint64_t timestamp = bios_proto_aux_number (bmessage, "time", ::time(NULL));
                // TODO: 2016-04-27 ACE: fix it later, when "string" values
                // in the metric would be considered as
                // normal behaviour, but for now it is not supposed to be so
                // -> generated error messages into the log
                char *end;
                double dvalue = strtod (value, &end);
                if (errno == ERANGE) {
                    errno = 0;
                    bios_proto_print (bmessage);
                    zsys_error ("cannot convert value to double #1, ignore message");
                    bios_proto_destroy (&bmessage);
                    continue;
                }
                else if (end == value || *end != '\0') {
                    bios_proto_print (bmessage);
                    zsys_error ("cannot convert value to double #2, ignore message");
                    bios_proto_destroy (&bmessage);
                    continue;
                }

                zsys_debug1("Got message '%s' with value %s", topic.c_str(), value);

                uint64_t ts_start = static_cast<uint64_t> (zclock_mono ());
                // Update cache with new value
                MetricInfo m (element_src, type, unit, dvalue, timestamp, "", ttl);
                bios_proto_destroy(&bmessage);
                cache.addMetric (m);
                cache.removeOldMetrics();
                evaluate_metric(client, m, cache, alertConfiguration);
                uint64_t ts_end = static_cast<uint64_t> (zclock_mono ());
                stream_messages.push_back (ts_end - ts_start);
            }
            bios_proto_destroy (&bmessage);
        }
        else if ( streq (mlm_client_command (client), "MAILBOX DELIVER" ) )
        {
            uint64_t ts_start = static_cast<uint64_t> (zclock_mono ());
            zsys_debug1 ("not bios_proto && mailbox");
            // According RFC we expect here a messages
            // with the topics ACK_SUBJECT and RULE_SUBJECT
            if ( streq (mlm_client_subject (client), RULES_SUBJECT) )
            {
                zsys_debug1 ("%s", RULES_SUBJECT);
                // Here we can have:
                //  * new rule
                //  * request for list of rules
                //  * get detailed info about the rule
                char *command = zmsg_popstr (zmessage);
                char *param = zmsg_popstr (zmessage);
                if (command && param) {
                    if (streq (command, "LIST")) {
                        list_rules (client, param, alertConfiguration);
                    }
                    else if (streq (command, "GET")) {
                        get_rule (client, param, alertConfiguration);
                    }
                    else if (streq (command, "ADD") ) {
                        if ( zmsg_size(zmessage) == 0 ) {
                            // ADD/json
                            add_rule (client, param, alertConfiguration);
                        }
                        else
                        {
                            // ADD/json/old_name
                            char *param1 = zmsg_popstr (zmessage);
                            update_rule (client, param, param1, alertConfiguration);
                            if (param1) free (param1);
                        }
                    }
                }
                zstr_free (&command);
                zstr_free (&param);
            }
            
            uint64_t ts_end = static_cast<uint64_t> (zclock_mono ());
            mailbox_messages.push_back (ts_end - ts_start);
        }
        zmsg_destroy (&zmessage);
    }
exit:
    zpoller_destroy (&poller);
    mlm_client_destroy (&client);
}

//  --------------------------------------------------------------------------
//  Self test of this class.

static char*
s_readall (const char* filename) {
    FILE *fp = fopen(filename, "rt");
    if (!fp)
        return NULL;

    size_t fsize = 0;
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *ret = (char*) malloc (fsize * sizeof (char) + 1);
    if (!ret) {
        fclose (fp);
        return NULL;
    }
    memset ((void*) ret, '\0', fsize * sizeof (char) + 1);

    size_t r = fread((void*) ret, 1, fsize, fp);
    fclose (fp);
    if (r == fsize)
        return ret;

    free (ret);
    return NULL;
}


void
bios_alert_generator_server_test (bool verbose)
{
    printf (" * bios_alert_generator_server: ");
    if (verbose)
        printf ("\n");

    int r = system ("rm -f src/*.rule");
    assert (r == 0); // to make gcc @ CentOS 7 happy

    //  @selftest
    static const char* endpoint = "inproc://bios-ag-server-test";

    zactor_t *server = zactor_new (mlm_server, (void*) "Malamute");
    zstr_sendx (server, "BIND", endpoint, NULL);
    if (verbose)
        zstr_send (server, "VERBOSE");

    mlm_client_t *producer = mlm_client_new ();
    mlm_client_connect (producer, endpoint, 1000, "producer");
    mlm_client_set_producer (producer, "METRICS");

    mlm_client_t *consumer = mlm_client_new ();
    mlm_client_connect (consumer, endpoint, 1000, "consumer");
    mlm_client_set_consumer (consumer, "_ALERTS_SYS", ".*");

    mlm_client_t *ui = mlm_client_new ();
    mlm_client_connect (ui, endpoint, 1000, "UI");

    zactor_t *ag_server = zactor_new (bios_alert_generator_server, (void*) "alert-agent");
    if (verbose)
        zstr_send (ag_server, "VERBOSE");
    zstr_sendx (ag_server, "CONNECT", endpoint, NULL);
    zstr_sendx (ag_server, "CONSUMER", "METRICS", ".*", NULL);
    zstr_sendx (ag_server, "PRODUCER", "_ALERTS_SYS", NULL);
    zstr_sendx (ag_server, "CONFIG", "src/", NULL);
    zclock_sleep (500);   //THIS IS A HACK TO SETTLE DOWN THINGS

    // Test case #1: list w/o rules
    zmsg_t *command = zmsg_new ();
    zmsg_addstrf (command, "%s", "LIST");
    zmsg_addstrf (command, "%s", "all");
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &command);

    zmsg_t *recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    char * foo = zmsg_popstr (recv);
    assert (streq (foo, "LIST"));
    zstr_free (&foo);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "all"));
    zstr_free (&foo);
    zmsg_destroy (&recv);

    // Test case #2.1: add new rule
    zmsg_t *rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    char* simplethreshold_rule = s_readall ("testrules/simplethreshold.rule");
    assert (simplethreshold_rule);
    zmsg_addstrf (rule, "%s", simplethreshold_rule);
    zstr_free (&simplethreshold_rule);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    // Test case #2.2: add new rule with existing name
    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    simplethreshold_rule = s_readall ("testrules/simplethreshold.rule");
    assert (simplethreshold_rule);
    zmsg_addstrf (rule, "%s", simplethreshold_rule);
    zstr_free (&simplethreshold_rule);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "ERROR"));
    zstr_free (&foo);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "ALREADY_EXISTS"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    // Test case #3: list rules
    command = zmsg_new ();
    zmsg_addstrf (command, "%s", "LIST");
    zmsg_addstrf (command, "%s", "all");
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &command);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 3);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "LIST"));
    zstr_free (&foo);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "all"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    // Test case #2.3: existing rule: simplethreshold
    //                 existing rule: simplethreshold2
    //                 update simplethreshold2 with new name simplethreshold
    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    simplethreshold_rule = s_readall ("testrules/simplethreshold2.rule");
    assert (simplethreshold_rule);
    zmsg_addstrf (rule, "%s", simplethreshold_rule);
    zstr_free (&simplethreshold_rule);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    simplethreshold_rule = s_readall ("testrules/simplethreshold.rule");
    assert (simplethreshold_rule);
    zmsg_addstrf (rule, "%s", simplethreshold_rule);
    zstr_free (&simplethreshold_rule);
    zmsg_addstrf (rule, "%s", "simplethreshold2");
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "ERROR"));
    zstr_free (&foo);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "ALREADY_EXISTS"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    // Test case #4: list rules - not yet stored type
    command = zmsg_new ();
    zmsg_addstrf (command, "%s", "LIST");
    zmsg_addstrf (command, "%s", "single");
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &command);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "LIST"));
    zstr_free (&foo);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "single"));
    zstr_free (&foo);
    zmsg_destroy (&recv);

    //Test case #5: generate alert - below the treshold
    zmsg_t *m = bios_proto_encode_metric (
            NULL, "abc", "fff", "20", "X", 0);
    mlm_client_send (producer, "abc@fff", &m);

    recv = mlm_client_recv (consumer);

    assert (is_bios_proto (recv));
    bios_proto_t *brecv = bios_proto_decode (&recv);
    assert (streq (bios_proto_rule (brecv), "simplethreshold"));
    assert (streq (bios_proto_element_src (brecv), "fff"));
    assert (streq (bios_proto_state (brecv), "ACTIVE"));
    assert (streq (bios_proto_severity (brecv), "CRITICAL"));
    bios_proto_destroy (&brecv);

    // Test case #6: generate alert - resolved
    m = bios_proto_encode_metric (
            NULL, "abc", "fff", "42", "X", 0);
    mlm_client_send (producer, "abc@fff", &m);

    recv = mlm_client_recv (consumer);

    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (streq (bios_proto_rule (brecv), "simplethreshold"));
    assert (streq (bios_proto_element_src (brecv), "fff"));
    assert (streq (bios_proto_state (brecv), "RESOLVED"));
    bios_proto_destroy (&brecv);

    // Test case #6: generate alert - high warning
    m = bios_proto_encode_metric (
            NULL, "abc", "fff", "52", "X", 0);
    mlm_client_send (producer, "abc@fff", &m);

    recv = mlm_client_recv (consumer);

    assert (recv);
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (brecv);
    assert (streq (bios_proto_rule (brecv), "simplethreshold"));
    assert (streq (bios_proto_element_src (brecv), "fff"));
    assert (streq (bios_proto_state (brecv), "ACTIVE"));
    assert (streq (bios_proto_severity (brecv), "WARNING"));
    bios_proto_destroy (&brecv);

    // Test case #7: generate alert - high critical
    m = bios_proto_encode_metric (
            NULL, "abc", "fff", "62", "X", 0);
    mlm_client_send (producer, "abc@fff", &m);

    recv = mlm_client_recv (consumer);

    assert (recv);
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (brecv);
    assert (streq (bios_proto_rule (brecv), "simplethreshold"));
    assert (streq (bios_proto_element_src (brecv), "fff"));
    assert (streq (bios_proto_state (brecv), "ACTIVE"));
    assert (streq (bios_proto_severity (brecv), "CRITICAL"));
    bios_proto_destroy (&brecv);

    // Test case #8: generate alert - resolved again
    m = bios_proto_encode_metric (
            NULL, "abc", "fff", "42", "X", 0);
    mlm_client_send (producer, "abc@fff", &m);

    recv = mlm_client_recv (consumer);

    assert (recv);
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (brecv);
    assert (streq (bios_proto_rule (brecv), "simplethreshold"));
    assert (streq (bios_proto_element_src (brecv), "fff"));
    assert (streq (bios_proto_state (brecv), "RESOLVED"));
    bios_proto_destroy (&brecv);

    // Test case #9: generate alert - high again
    m = bios_proto_encode_metric (
            NULL, "abc", "fff", "62", "X", 0);
    mlm_client_send (producer, "abc@fff", &m);

    recv = mlm_client_recv (consumer);

    assert (recv);
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (brecv);
    assert (streq (bios_proto_rule (brecv), "simplethreshold"));
    assert (streq (bios_proto_element_src (brecv), "fff"));
    assert (streq (bios_proto_state (brecv), "ACTIVE"));
    assert (streq (bios_proto_severity (brecv), "CRITICAL"));
    bios_proto_destroy (&brecv);
          
    // Test case #11: generate alert - high again - after ACK-PAUSE
    m = bios_proto_encode_metric (
            NULL, "abc", "fff", "62", "X", 0);
    mlm_client_send (producer, "abc@fff", &m);


    recv = mlm_client_recv (consumer);


    assert (recv);
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (brecv);
    assert (streq (bios_proto_rule (brecv), "simplethreshold"));
    assert (streq (bios_proto_element_src (brecv), "fff"));
    assert (streq (bios_proto_state (brecv), "ACTIVE"));
    assert (streq (bios_proto_severity (brecv), "CRITICAL"));
    bios_proto_destroy (&brecv);

    // Test case #12: generate alert - resolved - after ACK-PAUSE
    m = bios_proto_encode_metric (
            NULL, "abc", "fff", "42", "X", 0);
    mlm_client_send (producer, "abc@fff", &m);

    
    recv = mlm_client_recv (consumer);
    

    assert (recv);
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (brecv);
    assert (streq (bios_proto_rule (brecv), "simplethreshold"));
    assert (streq (bios_proto_element_src (brecv), "fff"));
    assert (streq (bios_proto_state (brecv), "RESOLVED"));
    bios_proto_destroy (&brecv);

    // Test case #13: segfault on onbattery
    // #13.1 ADD new rule
    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    char* onbattery_rule = s_readall ("testrules/onbattery-5PX1500-01.rule");
    assert (onbattery_rule);
    zmsg_addstrf (rule, "%s", onbattery_rule);
    zstr_free (&onbattery_rule);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);


    recv = mlm_client_recv (ui);


    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    // #13.2 evaluate metric
    m = bios_proto_encode_metric (
            NULL, "status.ups", "5PX1500-01", "1032.000", "", ::time (NULL));
    mlm_client_send (producer, "status.ups@5PX1500-01", &m);

    // Test case #14: add new rule, but with lua syntax error
    rule = zmsg_new();
    assert(rule);
    zmsg_addstrf (rule, "%s", "ADD");
    char* complexthreshold_rule_lua_error = s_readall ("testrules/complexthreshold_lua_error.rule");
    assert (complexthreshold_rule_lua_error);
    zmsg_addstrf (rule, "%s", complexthreshold_rule_lua_error);
    zstr_free (&complexthreshold_rule_lua_error);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);


    recv = mlm_client_recv (ui);


    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "ERROR"));
    zstr_free (&foo);
    foo = zmsg_popstr(recv);
    assert (streq (foo, "BAD_LUA"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    // Test case #15.1: add Radek's testing rule
    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    char* toohigh_rule = s_readall ("testrules/too_high-ROZ.ePDU13.rule");
    assert (toohigh_rule);
    zmsg_addstrf (rule, "%s", toohigh_rule);
    zstr_free (&toohigh_rule);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    zmsg_destroy (&recv);

    // Test case #15.2: evaluate it
    m = bios_proto_encode_metric (
            NULL, "status.ups", "ROZ.UPS33", "42.00", "", ::time (NULL));
    mlm_client_send (producer, "status.ups@ROZ.UPS33", &m);

    recv = mlm_client_recv (consumer);

    assert (recv);
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (brecv);
    assert (streq (bios_proto_rule (brecv), "too_high-ROZ.ePDU13"));
    assert (streq (bios_proto_element_src (brecv), "ePDU13"));
    assert (streq (bios_proto_state (brecv), "ACTIVE"));
    assert (streq (bios_proto_severity (brecv), "CRITICAL"));
    bios_proto_destroy (&brecv);

    // Test case #15.3: evaluate it again
    m = bios_proto_encode_metric (
            NULL, "status.ups", "ROZ.UPS33", "42.00", "", ::time (NULL));
    mlm_client_send (producer, "status.ups@ROZ.UPS33", &m);

    recv = mlm_client_recv (consumer);

    assert (recv);
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (brecv);
    assert (streq (bios_proto_rule (brecv), "too_high-ROZ.ePDU13"));
    assert (streq (bios_proto_element_src (brecv), "ePDU13"));
    assert (streq (bios_proto_state (brecv), "ACTIVE"));
    assert (streq (bios_proto_severity (brecv), "CRITICAL"));
    bios_proto_destroy (&brecv);
    zmsg_destroy (&recv);

    // Test case #16.1: add new rule, with the trash at the end
    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    char* rule_with_trash = s_readall ("testrules/rule_with_trash.rule");
    assert (rule_with_trash);
    zmsg_addstrf (rule, "%s", rule_with_trash);
    zstr_free (&rule_with_trash);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    // Test case #16.2: add new rule, GET the rule with trash
    command = zmsg_new ();
    zmsg_addstrf (command, "%s", "GET");
    zmsg_addstrf (command, "%s", "rule_with_trash");
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &command);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    foo = zmsg_popstr (recv);
    std::stringstream s{foo};
    cxxtools::JsonDeserializer d{s};
    cxxtools::SerializationInfo si;
    d.deserialize (si);
    assert (si.memberCount () == 1);
    zstr_free (&foo);
    zmsg_destroy (&recv);

    // test case #17 update the existing rule (type: threshold_simple)
    // input:
    //          * file check_update_threshold_simple.rule
    //          * file check_update_threshold_simple2.rule
    //      rules inside the files have the same names, but
    //      "values" are different
    // 1. add rule from the file check_update_threshold_simple.rule
    // 2. update "check_update_threshold_simple" rule with file "check_update_threshold_simple2.rule"
    //
    // expected result: SUCCESS

    // 1.
    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    simplethreshold_rule = s_readall ("testrules/check_update_threshold_simple.rule");
    assert (simplethreshold_rule);
    zmsg_addstrf (rule, "%s", simplethreshold_rule);
    zstr_free (&simplethreshold_rule);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    // 2.
    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    simplethreshold_rule = s_readall ("testrules/check_update_threshold_simple2.rule");
    assert (simplethreshold_rule);
    zmsg_addstrf (rule, "%s", simplethreshold_rule);
    zstr_free (&simplethreshold_rule);
    zmsg_addstrf (rule, "%s", "check_update_threshold_simple");
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);

    // check the result of the operation
    recv = mlm_client_recv (ui);
    
    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    
    zsys_info ("######## Test case #18 add some rule (type: pattern)");
    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    char* pattern_rule = s_readall ("testrules/pattern.rule");
    assert (pattern_rule);
    zmsg_addstrf (rule, "%s", pattern_rule);
    zstr_free (&pattern_rule);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);

    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);

    zsys_info ("######## Test case #19 evaluate some rule (type: pattern)");
    //      1. OK
    m = bios_proto_encode_metric (
            NULL, "end_warranty_date", "UPS_pattern_rule", "100", "some description", 24*60*60);
    mlm_client_send (producer, "end_warranty_date@UPS_pattern_rule", &m);

    //      1.1. No ALERT should be generated
    zpoller_t *poller = zpoller_new (mlm_client_msgpipe(consumer), NULL);
    void *which = zpoller_wait (poller, 1000);
    assert ( which == NULL );
    if ( verbose ) {
        zsys_debug ("No email was sent: SUCCESS");
    }
    zpoller_destroy (&poller);

    //      2. LOW_WARNING
    m = bios_proto_encode_metric (
            NULL, "end_warranty_date", "UPS_pattern_rule", "20", "some description", 24*60*60);
    mlm_client_send (producer, "end_warranty_date@UPS_pattern_rule", &m);

    recv = mlm_client_recv (consumer);
    assert ( recv != NULL );
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (streq (bios_proto_rule (brecv), "warranty"));
    assert (streq (bios_proto_element_src (brecv), "UPS_pattern_rule"));
    assert (streq (bios_proto_state (brecv), "ACTIVE"));
    assert (streq (bios_proto_severity (brecv), "WARNING"));
    bios_proto_destroy (&brecv);

    //      3. LOW_CRITICAL
    m = bios_proto_encode_metric (
            NULL, "end_warranty_date", "UPS_pattern_rule", "2", "some description", 24*60*60);
    mlm_client_send (producer, "end_warranty_date@UPS_pattern_rule", &m);

    recv = mlm_client_recv (consumer);
    assert ( recv != NULL );
    assert (is_bios_proto (recv));
    brecv = bios_proto_decode (&recv);
    assert (streq (bios_proto_rule (brecv), "warranty"));
    assert (streq (bios_proto_element_src (brecv), "UPS_pattern_rule"));
    assert (streq (bios_proto_state (brecv), "ACTIVE"));
    assert (streq (bios_proto_severity (brecv), "CRITICAL"));
    bios_proto_destroy (&brecv);
    
    zstr_free (&foo);
    zstr_free (&pattern_rule);
    zmsg_destroy (&recv);
    // Test case #20 update some rule (type: pattern)
/*  ACE: need help. here is some memory leak in the memcheck, cannot find
    rule = zmsg_new();
    zmsg_addstrf (rule, "%s", "ADD");
    pattern_rule = s_readall ("testrules/pattern.rule");
    assert (pattern_rule);
    zmsg_addstrf (rule, "%s", pattern_rule);
    zmsg_addstrf (rule, "%s", "warranty");
    zstr_free (&pattern_rule);
    mlm_client_sendto (ui, "alert-agent", "rfc-evaluator-rules", NULL, 1000, &rule);
    recv = mlm_client_recv (ui);

    assert (zmsg_size (recv) == 2);
    foo = zmsg_popstr (recv);
    assert (streq (foo, "OK"));
    zstr_free (&foo);
    // does not make a sense to call streq on two json documents
    zmsg_destroy (&recv);
*/
    // no new alert sent here
    zclock_sleep (3000);
    zactor_destroy (&ag_server);
    mlm_client_destroy (&ui);
    mlm_client_destroy (&consumer);
    mlm_client_destroy (&producer);
    zactor_destroy (&server);
    //  @end
    printf ("OK\n");

}
