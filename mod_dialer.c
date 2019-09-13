/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 * Neal Horman <neal at wanlink dot com>
 *
 *
 * mod_dialer.c -- Framework Demo Module
 *
 *
 *
 *
 *
 *
 * You need to enable ODBC in the core by installing unixodbc-dev and 
 * running ./configure && make && make install, creating your DSN as per
 * https://wiki.freeswitch.org/wiki/Using_ODBC_in_the_core
 *
 */
#include <switch.h>
#include <unistd.h>


static const char *global_cf = "dialer.conf";

typedef enum {
    RANDOM = 1,
    SEQUENTIAL = 2
} calling_strategy;

/* Defines */
#define MAX_CAMPAIGNS 10

struct db_campaign_config {
    char campaign_requested[50];
    char name[50];
    char datetime_start[20]; // Format must be %Y-%m-%d-%H-%M-%S"
    char dialplan_type[25];
    char context[50];
    char custom_header_name[150];
    char custom_header_value[100];
    char transfer_on_answer[50];
    switch_bool_t running;
    switch_bool_t stop;
    int max_concurrent_calls;
    unsigned long int time_between_calls;
    int attempts_per_number;
    int time_between_retries;
    int gaussian_distribution;
    int gaussian_distribution_mean;
    int gaussian_distribution_stdv;
    int call_max_duration;
    int call_min_duration;
    int originate_timeout;
    int cancel_ratio;
    char global_caller_id[50];
    char action_on_anwser[255];
    char destination_list[50];
    char codec_list[50];
    char profile_gateway[50];
    int calling_strategy;
    char my_local_ip[16];
    char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
    int current_calls;
    int finish_on;
    unsigned long int calls_made;
    unsigned long int answered;
    unsigned long int total_seconds;
    switch_memory_pool_t *pool;
    switch_mutex_t *mutex;
};

static struct {
    int debug;
    char *odbc_dsn;
    char *dbname;
    struct db_campaign_config campaigns[MAX_CAMPAIGNS];
    switch_bool_t running;
    switch_mutex_t *mutex;
    switch_memory_pool_t *pool;
} globals;

struct randnorm_state {
    unsigned int seed;
    float y2;
    int s;
};

/* Prototypes */
static void *SWITCH_THREAD_FUNC dialer_start_campaign(switch_thread_t *thread, void *obj);
static void dialer_event_handler(switch_event_t *event);
static void dialer_show_campaigns( const char * campaign );

static void dialer_clear_struct_slot(int index);
static int dialer_is_campaign_running( char * campaign_name );
static int dialer_dests_callback(void *pArg, int argc, char **argv, char **columnNames);
static int dialer_get_empty_index(struct db_campaign_config ** found_campaign, const char * campaign_requested );
static int dialer_get_campaign_by_uuid( const char * uuid );
static int dialer_get_campaign_by_name( const char * campaign_requested );
static switch_bool_t dialer_delete_campaign( const char * campaign_to_delete );
static char * dialer_get_event_header( switch_event_t *event, const char * header_name );
static switch_bool_t dialer_delete_all_campaigns();

void randnorm_init(struct randnorm_state* rs, unsigned int seed);
static float randnorm_r(struct randnorm_state* rs, float mean, float stddev);

static switch_bool_t dialer_set_number_inuse( int campaign_index, const char *number, const char * status );
static switch_bool_t dialer_increment_number_calls( int campaign_index, const char *number );

static switch_bool_t dialer_stop_campaign( const char * campaign_to_stop );
static switch_cache_db_handle_t *dialer_get_db_handle(void);
static switch_bool_t dialer_execute_sql_callback( switch_mutex_t *mutex, char *sql, switch_core_db_callback_func_t callback, void *pdata);

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dialer_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_dialer_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_dialer_load);

char destinations_sql[512];
char destinations_sql_format[] = "CREATE TABLE %s (\n"
                                 "   number	     VARCHAR(30) NOT NULL  PRIMARY KEY,\n"
                                 "   lastcall	     DATETIME,\n"
                                 "   lastresult     VARCHAR(30),\n"
                                 "   calls	     INT,\n"
                                 "   in_use		 INT,\n"
                                 "   duration    INT,\n"
                                 "   callerid    VARCHAR(25) NULL DEFAULT NULL,\n"
                                 "INDEX by_last_called (lastcall)\n"
                                 ") Engine=MyISAM ;\n";

char destinations_check_sql[100];
char destinations_check_format[] = "select count(*) from %s;";

char destinations_delete_sql[100];
char destinations_delete_format[] = "drop table %s;";


/* SWITCH_MODULE_DEFINITION(name, load, shutdown, runtime)
 * Defines a switch_loadable_module_function_table_t and a static const char[] modname
 */
SWITCH_MODULE_DEFINITION(mod_dialer, mod_dialer_load, mod_dialer_shutdown, NULL);

static void *SWITCH_THREAD_FUNC dialer_start_campaign(switch_thread_t *thread, void *obj)
{
    int campaign_index = (intptr_t) obj;
    struct db_campaign_config *job = &globals.campaigns[ campaign_index ];
    switch_memory_pool_t *pool;
    switch_bool_t campaign_found = SWITCH_FALSE;

    /* Campaign-related vars */
    switch_xml_t xml = NULL, cfg = NULL, x_campaigns = NULL, param = NULL, x_campaign = NULL;
    int params_set = 0;
    switch_cache_db_handle_t *dbh = NULL;

    /* sql stuff */
    char sql[1024];
    int rows_affected = NULL;


    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: start_camapign received index %d\n", campaign_index );
    pool = job->pool;

    /* We must lock the campaign */
    switch_mutex_lock(globals.mutex);
switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: LOCKING globals.mutex\n");


    job->running = 1;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Attempting to load campaign %s\n", job->campaign_requested );

    /* Try and load the cammpaign's from xml config file - Start */

    if (!(xml = switch_xml_open_cfg(global_cf, &cfg, NULL))) {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Couldn't load config file\n" );
        goto end;
    } else {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Opened XML config file\n" );
    }

    if ( !(x_campaigns = switch_xml_child(cfg, "campaigns"))) {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Couldn't find 'campaigns' section, exiting\n" );
        goto end;
    } else {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Found 'campaigns' section\n" );
    }

    job->stop = SWITCH_FALSE;

    for (x_campaign = switch_xml_child(x_campaigns, "campaign"); x_campaign; x_campaign = x_campaign->next) {
        const char *campaign_name = switch_xml_attr(x_campaign, "name");
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: in config: %s, requested: %s...\n", campaign_name, job->campaign_requested );
        if ( strcmp( job->campaign_requested, campaign_name) == 0 ) {
        	campaign_found = SWITCH_TRUE;
            switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Found Campaign: %s, requested: %s ...\n", campaign_name, job->campaign_requested );
            strncpy( job->name, campaign_name, sizeof(job->name) );

            /* here we will actually load the campaign data, then initialize the db using the campaign's destination_list value */
            for (param = switch_xml_child(x_campaign, "param"); param; param = param->next) {
                char *name = (char *) switch_xml_attr_soft(param, "name");
                char *value = (char *) switch_xml_attr_soft(param, "value");


                // Ignore empty/missing attributes
                if (switch_strlen_zero(name)) {
                    continue;
                }

                if (!strcmp(name, "datetime_start")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: datetime_start is: %s\n", value );
                    strncpy( job->datetime_start, value, sizeof(job->datetime_start) );
                    params_set++;
                } else if  (!strcmp(name, "context")) {
                    strncpy( job->context, value, sizeof(job->context) );
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: context is: %s (%s)\n", value, job->context );
                    params_set++;
                } else if  (!strcmp(name, "max_concurrent_calls")) {
                    job->max_concurrent_calls = atoi(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: max_concurrent_calls is: %i\n", job->max_concurrent_calls);
                    params_set++;
                } else if  (!strcmp(name, "add_custom_header_name")) {
                    strncpy( job->custom_header_name, value, sizeof(job->custom_header_name) );
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: add_custom_header_value is: %s\n", job->custom_header_name);
                    params_set++;
                } else if  (!strcmp(name, "add_custom_header_value")) {
                    strncpy( job->custom_header_value, value, sizeof(job->custom_header_value) );
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: add_custom_header_value is: %s\n", job->custom_header_value);
                    params_set++;
                } else if  (!strcmp(name, "time_between_calls")) {
                    job->time_between_calls = atol(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: time_between_calls is: %lu\n", job->time_between_calls);
                    params_set++;
                } else if  (!strcmp(name, "attempts_per_number")) {
                    job->attempts_per_number = atoi(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: attempts_per_number is: %i\n", job->attempts_per_number);
                    params_set++;
                } else if  (!strcmp(name, "time_between_retries")) {
                    job->time_between_retries = atoi(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: time_between_retries is: %i\n", job->time_between_retries);
                    params_set++;
                } else if  (!strcmp(name, "gaussian_distribution")) {
                    job->gaussian_distribution = atoi(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: gaussian_distribution is: %i\n", job->gaussian_distribution);
                    params_set++;
                } else if  (!strcmp(name, "gaussian_distribution_mean")) {
                    job->gaussian_distribution_mean = atoi(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: gaussian_distribution_mean is: %i\n", job->gaussian_distribution_mean);
                    params_set++;
                } else if  (!strcmp(name, "gaussian_distribution_stdv")) {
                    job->gaussian_distribution_stdv = atoi(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: gaussian_distribution_stdv is: %i\n", job->gaussian_distribution_stdv);
                    params_set++;
                } else if  (!strcmp(name, "call_max_duration")) {
                    job->call_max_duration = atoi(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: call_max_duration is: %i\n", job->call_max_duration);
                    params_set++;
                } else if  (!strcmp(name, "call_min_duration")) {
                    job->call_min_duration = atoi(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: call_min_duration is: %i\n", job->call_min_duration);
                    params_set++;
                } else if  (!strcmp(name, "originate_timeout")) {
                    job->originate_timeout = atoi(value);
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: originate_timeout is: %i\n", job->originate_timeout);
                    params_set++;
                } else if  (!strcmp(name, "global_caller_id")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: global_caller_id is: %s\n", value );
                    strncpy( job->global_caller_id, value, sizeof(job->global_caller_id) );
                    params_set++;
                } else if  (!strcmp(name, "action_on_anwser")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: action_on_anwser is: %s\n", value );
                    strncpy( job->action_on_anwser, value, sizeof(job->action_on_anwser) );
                    params_set++;
                } else if  (!strcmp(name, "destination_list")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: destination_list is: %s\n", value );
                    strncpy( job->destination_list, value, sizeof(job->destination_list) );
                    params_set++;
                } else if  (!strcmp(name, "codec_list")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: codec_list is: %s\n", value );
                    strncpy( job->codec_list, value, sizeof(job->codec_list) );
                    params_set++;
                } else if  (!strcmp(name, "profile/gateway")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: profile/gateway: %s\n", value );
                    strncpy( job->profile_gateway, value, sizeof(job->profile_gateway) );
                    params_set++;
                } else if  (!strcmp(name, "dialplan_type")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: dialplan_type: %s\n", value );
                    strncpy( job->dialplan_type, value, sizeof(job->dialplan_type) );
                    params_set++;
                } else if  (!strcmp(name, "transfer_on_answer")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: transfer_on_answer: %s\n", value );
                    strncpy( job->transfer_on_answer, value, sizeof(job->transfer_on_answer) );
                    params_set++;
                } else if  (!strcmp(name, "finish_on")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: finish_on: %s\n", value );
                    job->finish_on = atoi(value);
                    params_set++;
                } else if  (!strcmp(name, "custom_header_name")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: custom_header_name: %s\n", value );
                    strncpy( job->custom_header_name, value, sizeof(job->custom_header_name) );
                    params_set++;
                } else if  (!strcmp(name, "custom_header_value")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: custom_header_value: %s\n", value );
                    strncpy( job->custom_header_value, value, sizeof(job->custom_header_value) );
                    params_set++;
                } else if  (!strcmp(name, "cancel_ratio")) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: cancel_ratio: %s\n", value );
                    job->cancel_ratio = atoi(value);
                    params_set++;
                } else if ( !strcmp(name, "calling_strategy") ) {
                    if ( !strcmp(value, "random") ) {
                        job->calling_strategy = RANDOM;
                        params_set++;
                    } else  if ( !strcmp(value, "sequential") ) {
                        job->calling_strategy = SEQUENTIAL;
                        params_set++;
                    } else {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Unknown calling_strategy value: <%s>, must be <random> or <sequential>\n", value );
                        goto end;
                    }
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: calling_strategy: %i (1=random/2=sequential)\n", job->calling_strategy);
                }
                else
                {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Unknown attribute '%s' found in campaign '%s'\n", name, job->campaign_requested);
                }

            }
            if ( params_set != 24 ) {
                switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Couldn't find all parameters for campaign %s\n", campaign_name );
                goto end;
            }


            job->current_calls = 0;
            job->calls_made = 0;
            job->answered = 0;

            switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Done loading campaign %s, parameters set: %d\n", job->name, params_set );

        }
    }
    
    if ( campaign_found == SWITCH_FALSE ) {
    	switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "dialer: Campaign '%s' not found\n", job->campaign_requested );
    	job->running = SWITCH_FALSE;
    	dialer_delete_campaign( job->campaign_requested );
    	goto end;
    }

    /* Try and load the campaign's from xml config file - End */
    

    /* Initialize database and check if destinations table exists, else create the table */
    if (!(dbh = dialer_get_db_handle())) {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "dialer: Cannot open DB!\n" );
        goto end;
    } else {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Connected to db!\n" );
        sprintf(destinations_sql, destinations_sql_format,  job->destination_list);
        sprintf(destinations_check_sql, destinations_check_format,  job->destination_list);
        sprintf(destinations_delete_sql, destinations_delete_format,  job->destination_list);

        if ( !(switch_cache_db_test_reactive(dbh, destinations_check_sql, destinations_delete_sql, destinations_sql)))
        {
            goto end;
        } else {
            switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: destinations table looks good\n" );
        }
        switch_cache_db_release_db_handle(&dbh);
    }

    /* Finish loading the config */

    if (!(dbh = dialer_get_db_handle())) {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "dialer: Cannot open DB!\n" );
        goto end;
    }

    sprintf(sql, "select * from %s where in_use = 0 and calls < %d and ( time_to_sec( timediff ( now(), lastcall) ) > %d  or lastcall is NULL ) order by rand() LIMIT 1", job->destination_list, job->attempts_per_number, job->time_between_retries );
    switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: SQL: %s\n", sql );
    switch_mutex_unlock(globals.mutex);
switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");


    while ( job->stop == SWITCH_FALSE ) {
		switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: curr: %d - max: %d\n", job->current_calls, job->max_concurrent_calls );

    	while( job->current_calls < job->max_concurrent_calls && job->stop == SWITCH_FALSE ) {
			switch_mutex_lock(globals.mutex);
            switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: LOCKING globals.mutex\n");

			if( job->finish_on > 0 && globals.campaigns[ campaign_index ].calls_made >= job->finish_on ) {
	            switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: we've reached the amount of calls (%d) stopping now\n", job->finish_on );
				job->stop = SWITCH_TRUE;
			}

			switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Getting next number (%lu secs)...\n", job->time_between_calls );
			if( dialer_execute_sql_callback( NULL, sql, dialer_dests_callback, job->uuid_str ) == SWITCH_FALSE ) {
				goto end;
			}
			rows_affected = switch_cache_db_affected_rows( dbh );

			/* Make sure we're getting something from the table */
			if ( rows_affected == 0 ) {
				dialer_show_campaigns( job->name );
				dialer_clear_struct_slot( campaign_index );
				switch_mutex_unlock( globals.mutex );
				switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: We got 0 (ZERO) rows from the table, maybe it's empty?\n" );
				goto end;
			} else {
				switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: affected rows: %d\n", rows_affected );
				switch_mutex_unlock( globals.mutex );
				switch_yield( job->time_between_calls*1000*1000 );
			}
        }
        
        switch_yield( job->time_between_calls*1000*1000 );
    }
    
    switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: SOMETHING ENDED THE EXECUTION\n");
    
    /* wait for ongoing calls to end */
	while ( globals.campaigns[campaign_index].current_calls > 0 ) {
		switch_mutex_unlock(globals.mutex);
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");

		switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: stopping: Waiting for ongoing calls to end (campaign %d)\n", campaign_index );
		switch_yield( 2000000 );
		switch_mutex_lock(globals.mutex);
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: LOCKING globals.mutex\n");

	}        	

	switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: exiting from campaign %s\n", job->name );

    dialer_show_campaigns( job->name );
	dialer_clear_struct_slot( campaign_index );
    switch_mutex_unlock( globals.mutex );


end:
    /* destroy our thing, be tidy */
    job = NULL;
    if (pool) {
        switch_core_destroy_memory_pool(&pool);
    }
    pool = NULL;
    if (xml) {
        switch_xml_free(xml);
    }

	globals.campaigns[campaign_index].campaign_requested[0] = '\0';
    globals.campaigns[campaign_index].running = SWITCH_FALSE;
    switch_mutex_unlock(globals.mutex);
    switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKED globals.mutex and set running to SWITCH_FALSE\n");

    return NULL;
}

static void dialer_show_campaigns( const char * campaign )
{
    int campaign_index;
	switch_mutex_lock( globals.mutex );
    
    if ( !(strcmp( campaign , "all" ) ) ) {
        campaign_index = -1;
    } else {
        campaign_index = dialer_get_campaign_by_name( campaign );
        if ( campaign_index == -1 ) {
            return;
        }
    }
    
    if ( campaign_index == -1 ) {
        // We should initialize the comapign_config array inside our global struct
        for( int i=0; i<MAX_CAMPAIGNS; i++) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: ----------------------- Campaign %s -----------------------\n", campaign);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d campaign_requested: <%s>\n", i, globals.campaigns[i].campaign_requested);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d name: <%s>\n", i, globals.campaigns[i].name);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d datetime_start: <%s>\n", i, globals.campaigns[i].datetime_start);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d context: <%s>\n", i, globals.campaigns[i].context);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d running: <%d>\n", i, globals.campaigns[i].running);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d stop: <%d>\n", i, globals.campaigns[i].stop);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d max_concurrent_calls: <%d>\n", i, globals.campaigns[i].max_concurrent_calls);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d time_between_calls: <%lu>\n", i, globals.campaigns[i].time_between_calls);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d attempts_per_number: <%d>\n", i, globals.campaigns[i].attempts_per_number);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d time_between_retries: <%d>\n", i, globals.campaigns[i].time_between_retries);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d gaussian_distribution: <%d>\n", i, globals.campaigns[i].gaussian_distribution );
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d gaussian_distribution_mean: <%d>\n", i, globals.campaigns[i].gaussian_distribution_mean );
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d gaussian_distribution_stdv: <%d>\n", i, globals.campaigns[i].gaussian_distribution_stdv );
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d call_max_duration: <%d>\n", i, globals.campaigns[i].call_max_duration);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d call_min_duration: <%d>\n", i, globals.campaigns[i].call_min_duration);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d originate_timeout: <%d>\n", i, globals.campaigns[i].originate_timeout);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d cancel_ratio: <%d>\n", i, globals.campaigns[i].cancel_ratio);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d global_caller_id: <%s>\n", i, globals.campaigns[i].global_caller_id);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d action_on_anwser: <%s>\n", i, globals.campaigns[i].action_on_anwser);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d transfer_on_anwser: <%s>\n", i, globals.campaigns[i].transfer_on_answer);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d destination_list: <%s>\n", i, globals.campaigns[i].destination_list);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d codec_list: <%s>\n", i, globals.campaigns[i].codec_list);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d profile_gateway: <%s>\n", i, globals.campaigns[i].profile_gateway);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d calling_strategy: <%d>\n", i, globals.campaigns[i].calling_strategy);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d my_local_ip: <%s>\n", i, globals.campaigns[i].my_local_ip);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d uuid_str: <%s>\n", i, globals.campaigns[i].uuid_str);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d current_calls: <%d>\n", i, globals.campaigns[i].current_calls);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d calls_made: <%lu>\n", i, globals.campaigns[i].calls_made);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d answered: <%lu>\n", i, globals.campaigns[i].answered);
	        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d total_seconds: <%lu>\n", i, globals.campaigns[i].total_seconds);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d finish_on: <%d>\n", i, globals.campaigns[i].finish_on);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d custom_header_name:  <%s>\n", i, globals.campaigns[i].custom_header_name);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d custom_header_value:  <%s>\n", i, globals.campaigns[i].custom_header_value);
        }
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: ----------------------- Campaign Array #%s -----------------------\n", campaign );
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d campaign_requested: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].campaign_requested);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d name: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].name);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d datetime_start: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].datetime_start);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d context: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].context);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d running: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].running);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d stop: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].stop);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d max_concurrent_calls: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].max_concurrent_calls);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d time_between_calls: <%lu>\n", campaign_index, globals.campaigns[ campaign_index ].time_between_calls);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d attempts_per_number: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].attempts_per_number);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d time_between_retries: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].time_between_retries);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d gaussian_distribution: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].gaussian_distribution );
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d gaussian_distribution_mean: <%d>\n", campaign_index, globals.campaigns[campaign_index].gaussian_distribution_mean );
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d gaussian_distribution_stdv: <%d>\n", campaign_index, globals.campaigns[campaign_index].gaussian_distribution_stdv );
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d call_max_duration: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].call_max_duration);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d call_min_duration: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].call_min_duration);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d originate_timeout: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].originate_timeout);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d cancel_ratio: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].cancel_ratio);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d global_caller_id: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].global_caller_id);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d action_on_anwser: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].action_on_anwser);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d transfer_on_anwser: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].transfer_on_answer);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d destination_list: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].destination_list);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d codec_list: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].codec_list);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d profile_gateway: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].profile_gateway);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d calling_strategy: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].calling_strategy);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d my_local_ip: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].my_local_ip);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d uuid_str: <%s>\n", campaign_index, globals.campaigns[ campaign_index ].uuid_str);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d current_calls: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].current_calls);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d calls_made: <%lu>\n", campaign_index, globals.campaigns[ campaign_index ].calls_made);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d answered: <%lu>\n", campaign_index, globals.campaigns[ campaign_index ].answered);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d total_seconds: <%lu>\n", campaign_index, globals.campaigns[ campaign_index ].total_seconds);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d finish_on: <%d>\n", campaign_index, globals.campaigns[ campaign_index ].finish_on);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d custom_header_name:  <%s>\n", campaign_index, globals.campaigns[ campaign_index ].custom_header_name);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dialer: campaign %d custom_header_value:  <%s>\n", campaign_index, globals.campaigns[ campaign_index ].custom_header_value);
    }
    switch_mutex_unlock( globals.mutex );
}

#define LOG_SYNTAX "<action> [<test-name>] [<calls>]"
SWITCH_STANDARD_API(start_tests_function)
{
    char *argv[32] = { 0 };
    int argc, new_campaign_slot;
    char *mydata = NULL;
    switch_status_t status = SWITCH_STATUS_SUCCESS;

    struct db_campaign_config *my_campaign = NULL;
    switch_uuid_t uuid;
    char my_uuid[32];
    switch_memory_pool_t *pool;
    switch_thread_t *thread;
    switch_threadattr_t *thd_attr = NULL;

    if (zstr(cmd)) {
        goto usage;
    }

    mydata = strdup(cmd);
    switch_zmalloc( my_campaign, sizeof( struct db_campaign_config ) );

    if ((argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
        if (argc < 2) {
            goto usage;
        } else if  ( !strcmp(argv[0],"start") && !zstr(argv[1]) ) {

            /* Let's check if the requested campaign is already running somewhere */
            if ( dialer_is_campaign_running(argv[1]) > -1 ) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: The requested campaign is already running!\n" );
                status = SWITCH_STATUS_TERM;
                goto end;
            }

            /* We also need to find an empty campaign struct inside the globals' struct array so that we can insert the config there */
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Finding empty item in campaigns array\n" );
            new_campaign_slot = dialer_get_empty_index( &my_campaign, argv[1] );
            if ( new_campaign_slot > -1 ) {
                //if ( dialer_get_empty_slot( &my_campaign, argv[1] ) ) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: found a campaign %s index %d\n", argv[1], new_campaign_slot );
            } else {
                status = SWITCH_STATUS_TERM;
                goto end;
            }

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Got command %s for campaing %s... Attempting to start a new thread\n", argv[0], argv[1] );

            switch_core_new_memory_pool(&pool);
            my_campaign = &globals.campaigns[new_campaign_slot];
            my_campaign->pool = pool;
            strncpy( my_campaign->campaign_requested, switch_core_strdup(pool, argv[1]), sizeof( my_campaign->campaign_requested ) );
            
            

            switch_uuid_get( &uuid );
            switch_uuid_format( my_uuid, &uuid );
            strncpy( my_campaign->uuid_str, my_uuid, sizeof( my_campaign->uuid_str ) );

            switch_threadattr_create( &thd_attr, my_campaign->pool );
            switch_threadattr_detach_set(thd_attr, 1);
            switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
            stream->write_function(stream, "+OK Campaign-UUID: %s for campaign: %s\n", my_campaign->uuid_str, my_campaign->campaign_requested);

            switch_thread_create(&thread, thd_attr, dialer_start_campaign, (void *) (intptr_t) new_campaign_slot, my_campaign->pool);

            status = SWITCH_STATUS_SUCCESS;

            goto end;

        } else if  ( !strcmp(cmd,"status") ) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Got command %s\n", cmd );
            goto end;
        } else if  ( !strcmp(argv[0],"stop")  && !zstr(argv[1]) ) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Got command %s\n", cmd );

            if ( !dialer_stop_campaign( argv[1] ) ) {
            	status = SWITCH_STATUS_SUCCESS;
            } else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: stopped campaign %s\n", argv[1] );
                status = SWITCH_STATUS_TERM;
				goto end;
            }
        } else if  ( !strcmp(argv[0],"show") && !zstr(argv[1]) ) {
            dialer_show_campaigns( argv[1] );
            goto end;
        } else if  ( !strcmp(argv[0],"delete") && !zstr(argv[1]) ) {
            if ( dialer_delete_campaign( argv[1] ) == SWITCH_TRUE ) {
                status = SWITCH_STATUS_SUCCESS;
            }
            goto end;
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Unknown command <%s>\n", cmd );
            goto usage;
        }
    }

usage:
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Please specify command [start|stop|pause] (Got %s)\n", mydata );

end:
    switch_safe_free(mydata);
    return status;
}

/* Macro expands to: switch_status_t mod_dialer_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool) */
SWITCH_MODULE_LOAD_FUNCTION(mod_dialer_load)
{
    /* Vars for xml setting loading */
    switch_xml_t cfg = NULL, settings, xml = NULL, param = NULL;

    /* Other vars */
    switch_api_interface_t *dialer_api_interface;
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    memset(&globals, 0, sizeof(globals));
    globals.pool = pool;

    switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, globals.pool);

    /* connect my internal structure to the blank pointer passed to me */
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Starting dialer mod!\n");

    /* bind to events */
    if (switch_event_bind("mod_dialer", SWITCH_EVENT_HEARTBEAT, SWITCH_EVENT_SUBCLASS_ANY, dialer_event_handler, NULL ) !=  SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Couldn't bind event to monitor for session heartbeats!\n");
        return SWITCH_STATUS_GENERR;
    }

    if (switch_event_bind("mod_dialer", SWITCH_EVENT_CUSTOM, SWITCH_EVENT_SUBCLASS_ANY, dialer_event_handler, NULL ) !=  SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Couldn't bind event to monitor for CUSTOM event!\n");
        return SWITCH_STATUS_GENERR;
    } 

    if (switch_event_bind("mod_dialer", SWITCH_EVENT_CHANNEL_HANGUP_COMPLETE, SWITCH_EVENT_SUBCLASS_ANY, dialer_event_handler, NULL ) !=  SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Couldn't bind to SWITCH_EVENT_CHANNEL_HANGUP_COMPLETE!\n");
        return SWITCH_STATUS_GENERR;
    }

    if (switch_event_bind("mod_dialer", SWITCH_EVENT_CHANNEL_ANSWER, SWITCH_EVENT_SUBCLASS_ANY, dialer_event_handler, NULL ) !=  SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Couldn't bind to SWITCH_EVENT_CHANNEL_ANSWER!\n");
        return SWITCH_STATUS_GENERR;
    }

    /* set api commands */
    if (switch_true(switch_core_get_variable("disable_system_api_commands"))) {
        //use_system_commands = 0;
    }

    /* Load global settings into global struct - Start*/
    if (!(xml = switch_xml_open_cfg(global_cf, &cfg, NULL)))
    {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Couldn't load config file\n" );
        goto end;
    }

    if ( !(settings = switch_xml_child(cfg, "settings")) ) {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Couldn't find 'settings' section, exiting\n" );
        status = SWITCH_STATUS_GENERR;
        goto end;
    } else {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Found 'settings' section\n" );
        /* mutex */
        switch_mutex_lock(globals.mutex);
switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: LOCKING globals.mutex\n");


        for (param = switch_xml_child(settings, "param"); param; param = param->next) {
            char *var = (char *) switch_xml_attr_soft(param, "name");
            char *val = (char *) switch_xml_attr_soft(param, "value");

            if (!strcasecmp(var, "debug")) {
                globals.debug = atoi(val);
            } else if (!strcasecmp(var, "dbname")) {
                globals.dbname = strdup(val);
                switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: dbname is: %s\n", globals.dbname );
            } else if (!strcasecmp(var, "odbc-dsn")) {
                globals.odbc_dsn = strdup(val);
                switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: odbc_dsn is: %s\n", globals.odbc_dsn );
            } else {
                switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Unkown parameter in 'settings': %s\n", var );
            }
        }
        
        if ( !zstr(globals.dbname) && !zstr(globals.odbc_dsn) ) {
            status = SWITCH_STATUS_SUCCESS;
        }

        switch_mutex_unlock(globals.mutex);
switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");

    }
    /* Load global settings into global struct - End */


    /* connect my internal structure to the blank pointer passed to me */
    SWITCH_ADD_API(dialer_api_interface, "dialer", "Start dialer", start_tests_function, "[start|status|stop]");

    /* Done setting api commands */
end:
    if (xml) {
        switch_xml_free(xml);
    }
    return status;

}

static char * dialer_get_event_header( switch_event_t *event, const char * header_name ) {
    for (switch_event_header_t *hp = event->headers; hp; hp = hp->next) {
        char *var = hp->name;
        char *val = hp->value;
        if ( !strcmp( var, header_name ) ) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: comparing %s with %s!\n", var, header_name );
            return val;
        }
    }
    return NULL;
}

/* You can turn on session heartbeat on a channel to have us check billing more often */
static void dialer_event_handler(switch_event_t *event)
{
    char *uuid;
    char *number;
    switch_event_header_t *hp;
    int campaign_index;
    
    /* Only if the event is ours */
    
    if ( !(switch_event_get_header(event, "variable_campaign_id") == NULL) ){
    
    	campaign_index =  atoi( switch_event_get_header(event, "variable_campaign_id") );

		if ( !strcmp( switch_event_name(event->event_id), "CHANNEL_ANSWER" ) ) {

			switch_mutex_lock(globals.mutex);
			switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: LOCKING globals.mutex\n");

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
				"dialer: incrementing answered for "
				"campaign_id %d: "
				"current calls: %d, "
				"total answered: %lu\n",
				campaign_index,
				globals.campaigns[ campaign_index ].current_calls,
				++globals.campaigns[ campaign_index ].answered );

			switch_mutex_unlock(globals.mutex);
			switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");
		} 


		if ( !strcmp( switch_event_name(event->event_id), "CHANNEL_HANGUP_COMPLETE" ) && !strcmp( dialer_get_event_header( event, "Call-Direction" ) , "outbound" ) ) {

			switch_mutex_lock(globals.mutex);
			switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: LOCKING globals.mutex\n");

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: decrementing current_calls for campaign_id %d\n", campaign_index );
			globals.campaigns[ campaign_index ].current_calls--;
			globals.campaigns[ campaign_index ].total_seconds += atoi( switch_event_get_header(event, "variable_duration") );
		
			number = switch_event_get_header(event, "Caller-Callee-ID-Number");
			if ( dialer_set_number_inuse( campaign_index, number, "0") == SWITCH_TRUE ) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: set number as not in use: %s\n", number );
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: I couldn't set is as in NOT in use: %s\n", number );
			}

			switch_mutex_unlock(globals.mutex);
			switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");
		}
	
    } else {
		switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: call is not ours\n");
    }

    if (!event) {
        /* We should never get here - it means an event came in without the event info */
        return;
    }

    /* Make sure everything is sane */
    if (!(uuid = switch_event_get_header(event, "Unique-ID"))) {
        /* Donde esta channel? */
        return;
    }

    for (hp = event->headers; hp; hp = hp->next) {
        char *var = hp->name;
        char *val = hp->value;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: header %s: %s\n", var, val );
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: Done Listing headers --------------------------\n" );
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Exiting even_handler for received event %s\n", switch_event_name(event->event_id));
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_dialer_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dialer_shutdown)
{
    /* Cleanup dynamically allocated config settings
    switch_channel_unbind_device_state_handler(mycb);
    switch_xml_config_cleanup(instructions); */

    /* stopping all campaigns */
	switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: LOCKING globals.mutex\n");

    for (int i=0; i<MAX_CAMPAIGNS; i++) {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: stopping all campaigns (%d)\n", i );
		switch_mutex_lock(globals.mutex);
        globals.campaigns[i].stop = SWITCH_TRUE;
		switch_mutex_unlock(globals.mutex);
    }
    
    for (int i=0; i<MAX_CAMPAIGNS; i++) {
        while ( globals.campaigns[i].running == SWITCH_TRUE ) {
            switch_mutex_unlock(globals.mutex);
        	switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Waiting for campaigns to finish\n");
            switch_yield(5000000);
            switch_mutex_lock(globals.mutex);
        }
    }
    
    if ( dialer_delete_all_campaigns() == SWITCH_FALSE ) {
		switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: could NOT delete all campaigns!\n");
    }
    
	switch_mutex_unlock(globals.mutex);
	switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");

    switch_event_unbind_callback(dialer_event_handler);
	switch_safe_free(globals.dbname);
	switch_safe_free(globals.odbc_dsn);

	switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: module unloaded\n");
    return SWITCH_STATUS_SUCCESS;
}

static switch_bool_t dialer_delete_all_campaigns() {
    
    for ( int i=0; i<MAX_CAMPAIGNS; i++ ) {
    	if ( !zstr(globals.campaigns[i].campaign_requested) ) {
    		if( dialer_delete_campaign( globals.campaigns[i].campaign_requested ) == SWITCH_FALSE ) {
    			switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: could NOT remove campaign %s\n", globals.campaigns[i].campaign_requested );
    			return SWITCH_FALSE;
    		}
    	} else {
			switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: campaign %s deleted\n", globals.campaigns[i].campaign_requested );
    	}
    }
	return SWITCH_TRUE;
}

/*!\brief Loop through campaign's array searching for the passed "char * campaign_name" in "campaigns[i].campaign_requested" and
 * return the index if found, -1 if not
 */
static int dialer_is_campaign_running( char * campaign_requested )
{
    for ( int i=0; i<MAX_CAMPAIGNS; i++ ) {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: checking [%d]: %s\n", i, globals.campaigns[i].campaign_requested);
        if ( !zstr(campaign_requested) ) {
            switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: looking for %s in campaign array [%d]: %s\n", campaign_requested, i, globals.campaigns[i].campaign_requested);
            if ( strcmp( globals.campaigns[i].campaign_requested, campaign_requested ) == 0 ) {
                return i;
            }
        }
    }
    return -1;
}

/*!\brief Loop through campaign's array searching for an empty "campaigns[i].campaign_requested", meaning such index is not being used
 * parameter `found_campaign` is a "struct db_campaign_config **", this will be pointing to the found slot if a slot was found.
 * parameter `campaign_requested` is the campaign to start, we will assign it to "globals.campaigns[i].campaign_requested" so that no other thread can use it
 * return SWITCH_TRUE if a slot was found and successfully assigned, SWITCH_FALSE otherwise
 */
static int dialer_get_empty_index(struct db_campaign_config ** found_campaign, const char * campaign_requested )
{
    for (int i=0; i<MAX_CAMPAIGNS; i++) {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Looking for campaign with empty 'campaign_requested' [%d]: <%s>\n", i, globals.campaigns[i].campaign_requested);
        if ( zstr( globals.campaigns[i].campaign_requested ) ) {
            return i;
        }
    }
    return -1;
}

static int dialer_get_campaign_by_name( const char * campaign_requested )
{
    for (int i=0; i<MAX_CAMPAIGNS; i++) {
        if ( !strcmp( globals.campaigns[i].campaign_requested, campaign_requested ) ) {
            return i;
        }
    }
    return -1;
}

static int dialer_get_campaign_by_uuid( const char * uuid )
{
    for (int i=0; i<MAX_CAMPAIGNS; i++) {
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Looking for campaign with empty 'campaign_requested' [%d]: <%s>\n", i, globals.campaigns[i].campaign_requested);
        if ( strcmp( globals.campaigns[i].uuid_str, uuid ) == 0 ) {
            return i;
        }
    }
    return -1;
}

static switch_bool_t dialer_stop_campaign( const char * campaign_to_stop  )
{
    switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: stopping campaign %s\n", campaign_to_stop );
    for ( int i=0; i<MAX_CAMPAIGNS; i++  ) {
        if ( !zstr(campaign_to_stop)  ) {
            if ( strcmp( globals.campaigns[i].campaign_requested, campaign_to_stop ) == 0  ) {
                switch_mutex_lock(globals.mutex);
switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: LOCKING globals.mutex\n");

                globals.campaigns[i].stop = SWITCH_TRUE;
                switch_mutex_unlock(globals.mutex);
switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");

                switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: stopped %s\n", campaign_to_stop );
                return SWITCH_TRUE;
            }
        }
    }
    switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Couldn't find campaign %s\n", campaign_to_stop );
    return SWITCH_FALSE;
}

static switch_bool_t dialer_delete_campaign( const char * campaign_to_delete )
{
    int campaign_index = dialer_get_campaign_by_name( campaign_to_delete );
    
    if( globals.campaigns[campaign_index].running == 0 ) {
        if ( campaign_index > -1 && campaign_index < MAX_CAMPAIGNS ) {
            globals.campaigns[campaign_index].campaign_requested[0] = '\0';
            globals.campaigns[campaign_index].datetime_start[0] = '\0';
            globals.campaigns[campaign_index].context[0] = '\0';
            globals.campaigns[campaign_index].custom_header_name[0] = '\0';
            globals.campaigns[campaign_index].custom_header_value[0] = '\0';
            globals.campaigns[campaign_index].max_concurrent_calls = 0;
            globals.campaigns[campaign_index].time_between_calls = 0;
            globals.campaigns[campaign_index].attempts_per_number = 0;
            globals.campaigns[campaign_index].time_between_retries = 0;
            globals.campaigns[campaign_index].gaussian_distribution = 0;
            globals.campaigns[campaign_index].gaussian_distribution_mean = 0;
            globals.campaigns[campaign_index].gaussian_distribution_stdv = 0;
            globals.campaigns[campaign_index].call_max_duration = 0;
            globals.campaigns[campaign_index].call_min_duration = 0;
            globals.campaigns[campaign_index].originate_timeout = 0;
            globals.campaigns[campaign_index].global_caller_id[0] = '\0';
            globals.campaigns[campaign_index].action_on_anwser[0] = '\0';
            globals.campaigns[campaign_index].destination_list[0] = '\0';
            globals.campaigns[campaign_index].codec_list[0] = '\0';
            globals.campaigns[campaign_index].uuid_str[0] = '\0';
            globals.campaigns[campaign_index].profile_gateway[0] = '\0';
            globals.campaigns[campaign_index].dialplan_type[0] = '\0';
            globals.campaigns[campaign_index].transfer_on_answer[0] = '\0';
            globals.campaigns[campaign_index].finish_on = 0;
            globals.campaigns[campaign_index].calling_strategy = '\0';
			globals.campaigns[campaign_index].cancel_ratio = 0;

        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: campaign <%s> not found\n", campaign_to_delete );
            return SWITCH_FALSE;
        }
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "dialer: deleted campaign %s\n", campaign_to_delete );
        return SWITCH_TRUE;
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: can't stop campaign <%s> is running\n", campaign_to_delete );
        return SWITCH_FALSE;
    }
    
}

static switch_cache_db_handle_t *dialer_get_db_handle(void)
{
    switch_cache_db_handle_t *dbh = NULL;
    char *dsn;

    if (!zstr(globals.odbc_dsn)) {
        dsn = globals.odbc_dsn;
    } else {
        dsn = globals.dbname;
    }

    if (switch_cache_db_get_db_handle_dsn(&dbh, dsn) != SWITCH_STATUS_SUCCESS) {
        dbh = NULL;
    }

    return dbh;

}

static switch_bool_t dialer_execute_sql_callback( switch_mutex_t *mutex, char *sql, switch_core_db_callback_func_t callback, void *pdata)
{
    switch_bool_t ret = SWITCH_FALSE;
    char *errmsg = NULL;
    switch_cache_db_handle_t *dbh = NULL;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: in dialer_execute_sql_callback function\n");

    if (mutex) {
        switch_mutex_lock(mutex);
    } else {
        switch_mutex_lock(globals.mutex);
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: LOCKING globals.mutex\n");
    }

    if (!(dbh = dialer_get_db_handle())) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Error Opening DB\n");
        goto end;
    }

    if ( switch_cache_db_execute_sql_callback(dbh, sql, callback, pdata, &errmsg) == SWITCH_STATUS_SUCCESS ) {
    	ret = SWITCH_TRUE;
    }

    if (errmsg) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: SQL ERR: [%s] %s\n", sql, errmsg);
        free(errmsg);
    }

end:

    switch_cache_db_release_db_handle(&dbh);

    if (mutex) {
        switch_mutex_unlock(mutex);
    } else {
        switch_mutex_unlock(globals.mutex);
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: going OUT of dialer_execute_sql_callback function\n");
    return ret;
}

static int dialer_dests_callback(void *pArg, int argc, char **argv, char **columnNames)
{
#define MAX_DIAL_STRING 256

    char *my_uuid = (char *) pArg;
    const char *number = argv[0];
    const char *lastcall = argv[1];
    const char *lastresult = argv[2];
    const char *calls = argv[3];
    const char *inuse = argv[4];
    int duration_in_table = atoi( argv[5] );
    const char *sql_get_numbers;
    const char *custom_header = NULL, *sched_duration = NULL;
    int rnd_number = 0;
    
    int campaign_index = -1;
    char *exten, *cid_name, *cid_num;
    
    switch_core_session_t *caller_session = NULL;
    switch_call_cause_t *ccause = SWITCH_CAUSE_NONE;
    uint32_t timeout = 60;

    switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;

    campaign_index = dialer_get_campaign_by_uuid( my_uuid );
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: in dialer_dests_callback function\n");

    //ORIGINATE_SYNTAX "<call url> <exten>|&<application_name>(<app_args>) [<dialplan>] [<context>] [<cid_name>] [<cid_num>] [<timeout_sec>]"

    if ( campaign_index > -1 && globals.campaigns[ campaign_index ].stop == SWITCH_TRUE ) {
        return 1;
    }

    if ( campaign_index < 0 ) {
        return -1;
    } else {
    
        struct randnorm_state rs;
    
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Number belongs to campaign %s with uuid: %s I got number: %s - lastcall: %s - lastresult: %s"" - calls: %s - inuse: %s\n" , globals.campaigns[ campaign_index ].name, my_uuid, number, lastcall, lastresult, calls, inuse );
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Originating call...\n" );

        /* set vars from campaign globals */
        exten = globals.campaigns[ campaign_index ].action_on_anwser;
        
        /* 	
        	If cancel_ratio > 0 then we just set the sched_hangup for 1 second, else
        	If gaussian is disabled, duration in table is 0 and max and min_call_duration is 0, we set no time limit 
        */
        
        rnd_number = ( rand() % ( 100 + 1 - 0 ) + 0 );
        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: Random Number: %d - cancel_ratio: %d\n", rnd_number, globals.campaigns[ campaign_index ].cancel_ratio );

        if ( globals.campaigns[ campaign_index ].cancel_ratio > 0 && rnd_number < globals.campaigns[ campaign_index ].cancel_ratio ) {
			sched_duration = switch_mprintf( "execute_on_pre_answer='sched_api +1 normal_clearing bleg'" );
	        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: ------------------>>>>>>> Random Number: %d - cancel_ratio: %d\n", rnd_number, globals.campaigns[ campaign_index ].cancel_ratio );
        } else {
			if ( globals.campaigns[ campaign_index ].gaussian_distribution == 0 ) {
				if ( duration_in_table == 0 ) {
					if ( globals.campaigns[ campaign_index ].call_max_duration == 0 && globals.campaigns[ campaign_index ].call_min_duration == 0 ) {
						sched_duration = "";
					} else {
						sched_duration = switch_mprintf( "execute_on_answer='sched_hangup +%d alloted_timeout'", rand() % ( globals.campaigns[ campaign_index ].call_max_duration + 1 - globals.campaigns[ campaign_index ].call_min_duration) + globals.campaigns[ campaign_index ].call_min_duration );
					}
				} else {
					sched_duration = switch_mprintf( "execute_on_answer='sched_hangup +%d alloted_timeout'", duration_in_table);
				}
			} else if ( globals.campaigns[ campaign_index ].gaussian_distribution_mean > 0 && globals.campaigns[ campaign_index ].gaussian_distribution_stdv > 0 ) {
				randnorm_init(&rs, time(NULL));
				sched_duration = switch_mprintf( "execute_on_answer='sched_hangup +%d alloted_timeout'", (int)randnorm_r(&rs, globals.campaigns[ campaign_index ].gaussian_distribution_mean, globals.campaigns[ campaign_index ].gaussian_distribution_stdv) );
			} else {
				switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: gaussian distribution is enabled, but there's no 'mean' or 'stdv', please set them or disable it!\n");
				return 0;
			}
	    }
	    
	    if ( !zstr( globals.campaigns[ campaign_index ].custom_header_name) && !zstr( globals.campaigns[ campaign_index ].custom_header_value) ) {
            custom_header = switch_mprintf("%s=%s,", globals.campaigns[ campaign_index ].custom_header_name, globals.campaigns[ campaign_index ].custom_header_value);
            switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: added custom header: %s -> %s\n", globals.campaigns[ campaign_index ].custom_header_name, globals.campaigns[ campaign_index ].custom_header_value);
	    }
	           
        sql_get_numbers = switch_mprintf(
            "{"
				"%s"
				"originate_timeout=%d,"
				"campaign_id=%d,"
				"origination_caller_id_name=%s,"
				"origination_caller_id_number=%s,"
				"absolute_codec_string='%s',"
				"%s"
            "}sofia/gateway/%s/%s", 
            custom_header,
            globals.campaigns[ campaign_index ].originate_timeout, 
            campaign_index,
            number,
            number,
            globals.campaigns[ campaign_index ].codec_list,
            sched_duration, 
            globals.campaigns[ campaign_index ].profile_gateway, 
            number 
        );

		// argv[6] is the incoming callerid from the sql query to the destinations table
		if ( strlen(argv[6]) == 0 ) {
        	cid_name = globals.campaigns[ campaign_index ].global_caller_id;
        	cid_num = globals.campaigns[ campaign_index ].global_caller_id;
        } else {
			cid_name = argv[6];
			cid_num = argv[6];
        }

        switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: dial_string: %s -> %s\n", sql_get_numbers, exten );

		globals.campaigns[ campaign_index ].calls_made++;
		globals.campaigns[ campaign_index ].current_calls++;
		if ( dialer_set_number_inuse( campaign_index, number, "1" ) == SWITCH_FALSE ) {
			switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: couldn't set the number as 'in-use' on the dbtable! cancelling...\n");
			return 1;
		}

        if (switch_ivr_originate(NULL, &caller_session, &cause, sql_get_numbers, timeout, NULL, cid_name, cid_num, NULL, NULL, 0, ccause) != SWITCH_STATUS_SUCCESS || !caller_session ) {
            switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: something went wrong when sending the call, skipping\n");
            dialer_set_number_inuse( campaign_index, number, "0" );
            return 0;
        } else {
            switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: call sent, with transfer: exten:%s, dialplan_type:%s, context: %s\n", globals.campaigns[ campaign_index ].transfer_on_answer , globals.campaigns[ campaign_index ].dialplan_type, globals.campaigns[ campaign_index ].context);
            if ( !dialer_increment_number_calls( campaign_index, number ) ) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: I couldn't increment the number's calls: %s\n", number );
            }
            switch_ivr_session_transfer(caller_session, globals.campaigns[ campaign_index ].transfer_on_answer , globals.campaigns[ campaign_index ].dialplan_type, globals.campaigns[ campaign_index ].context);
        }

        switch_core_session_rwunlock(caller_session);

    }
    return 0;
}

static void dialer_clear_struct_slot(int index)
{
    switch_mutex_unlock(globals.mutex);
	switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");

        globals.campaigns[index].campaign_requested[0] = '\0';
        globals.campaigns[index].name[0] = '\0';
        globals.campaigns[index].datetime_start[0] = '\0';
        globals.campaigns[index].context[0] = '\0';
        globals.campaigns[index].running = SWITCH_FALSE;
        globals.campaigns[index].stop = SWITCH_FALSE;
        globals.campaigns[index].max_concurrent_calls = 0;
        globals.campaigns[index].time_between_calls = 0;
        globals.campaigns[index].attempts_per_number = 0;
        globals.campaigns[index].time_between_retries = 0;
        globals.campaigns[index].call_max_duration = 0;
        globals.campaigns[index].call_min_duration = 0;
        globals.campaigns[index].originate_timeout = 0;
        globals.campaigns[index].global_caller_id[0] = '\0';
        globals.campaigns[index].action_on_anwser[0] = '\0';
        globals.campaigns[index].transfer_on_answer[0] = '\0';
        globals.campaigns[index].destination_list[0] = '\0';
        globals.campaigns[index].codec_list[0] = '\0';
        globals.campaigns[index].profile_gateway[0] = '\0';
        globals.campaigns[index].calling_strategy = 0;
        globals.campaigns[index].my_local_ip[0] = '\0';
        globals.campaigns[index].uuid_str[0] = '\0';
        globals.campaigns[index].current_calls = 0;
        globals.campaigns[index].calls_made = 0;
        globals.campaigns[index].answered = 0;
        globals.campaigns[index].total_seconds = 0;
        globals.campaigns[index].cancel_ratio = 0;
    switch_mutex_unlock(globals.mutex);
	switch_log_printf( SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: UN-LOCKING globals.mutex\n");

}

static switch_bool_t dialer_set_number_inuse( int campaign_index, const char *number, const char * status ) 
{
    char *errmsg = NULL;
    char *sql_update = NULL;
    switch_cache_db_handle_t *dbh = NULL;
    switch_bool_t ret = SWITCH_FALSE;
    
    if (!(dbh = dialer_get_db_handle())) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Error Opening DB\n");
        ret = SWITCH_FALSE;
        goto end;
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "update %s set lastcall=NOW(), inuse = 1 where number = '%s';", globals.campaigns[ campaign_index ].destination_list, number );
    sql_update = switch_mprintf( "update %s set lastcall=NOW(), in_use = %s where number = '%s';", globals.campaigns[ campaign_index ].destination_list, status, number );
    switch_cache_db_execute_sql( dbh, sql_update, &errmsg );

    if (errmsg) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: SQL ERR: [%s] %s\n", sql_update, errmsg);
        free(errmsg);
    } else {
        ret = SWITCH_TRUE;
    }
    
end:

    switch_cache_db_release_db_handle(&dbh);

	if ( ret == SWITCH_TRUE ) {
    	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: going OUT of dialer_set_number_inuse function - TRUE - ALL OK\n");
	} else {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: going OUT of dialer_set_number_inuse function - FALSE - NOT OK\n");
	}

    return ret;
}

static switch_bool_t dialer_increment_number_calls( int campaign_index, const char *number ) 
{
    char *errmsg = NULL;
    char *sql_update = NULL;
    switch_cache_db_handle_t *dbh = NULL;
    switch_bool_t ret = SWITCH_FALSE;
    
    if (!(dbh = dialer_get_db_handle())) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: Error Opening DB\n");
        goto end;
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "update %s set inuse = 1 where number = '%s';", globals.campaigns[ campaign_index ].destination_list, number );
    sql_update = switch_mprintf( "update %s set calls = calls + 1 where number = '%s';", globals.campaigns[ campaign_index ].destination_list, number );
    switch_cache_db_execute_sql( dbh, sql_update, &errmsg );

    if (errmsg) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dialer: SQL ERR: [%s] %s\n", sql_update, errmsg);
        free(errmsg);
    } else {
        ret = SWITCH_TRUE;
    }
    
end:

    switch_cache_db_release_db_handle(&dbh);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dialer: going OUT of dialer_set_number_inuse function\n");
    return ret;

}


// This generates a random number using Gaussian distribution using the provided
// mean and standard-deviation
// you need to initialise the random struct like: randnorm_init(&rs, time(NULL));

// Contributed by Camille Oudot camille.oudot@gmail.com

void randnorm_init(struct randnorm_state* rs, unsigned int seed) 
{
    rs->seed = seed;
    rs->s = 0;
}

static float randnorm_r(struct randnorm_state* rs, float mean, float stddev) 
{
    float x1, x2, w, y1;
    
    if (rs->s == 1) {
        rs->s = 0;
        return rs->y2 * stddev + mean;
    }
    
    do {
        x1 = (float)rand_r(&rs->seed) / ((float)RAND_MAX / 2.) - 1.;
        x2 = (float)rand_r(&rs->seed) / ((float)RAND_MAX / 2.) - 1.;
        w = x1 * x1 + x2 * x2;
    } while (w >= 1. || w == 0.);

    w = sqrt((-2. * log(w)) / w);
    y1 = x1 * w;
    rs->y2 = x2 * w;
    rs->s = 1;

    return y1 * stddev + mean;
}
