# mod_dialer
freeSWITCH mod_dialer

I had a need to make mass-calls and when connected, send them to an IVR, message or whatever.
I couldn't find any suitable solution out there, so i created this freeSWITCH module.

You'd need to configure ODBC in the core. The module creates its db table automatically at start if not found.
You'd need to add the numbers to call to the table and configure your campaign as follows in the xml config file.

| Parameter     | Description   |
| ------------- |:-------------:|
| **context** |  What context to send the connected call, default is "default"|
| **profile/gateway** | Gateway (already configured) to use when calling out.|
| **add_custom_header_name** | Additional header Name to add to the INVITE, default "sip_h_P-Campaing-Name"|
| **add_custom_header_value** | Additional header value, default "my_campaign"|
| **max_concurrent_calls** | Maximum concurrent calls.|
| **time_between_calls** | Time in seconds to wait before sending the next call.|
| **attempts_per_number** | How many times to attempt to call a number before jumping to the next one.|
| **time_between_retries** | Time to wait before retrying the failed number.
| **originate_timeout** | How long to wait before giving up on outbound calls to be answered. Default 30 |
| **cancel_ratio** | For stress-tests, to try to reproduce a more real-world scenario, let's cancel this % of outbound calls. Default 50 |
| **global_caller_id** | If the number row's field in the db table 'callerid' is empty, we will use the following as callerid |  
| **destination_list** | Name of the db table containing the numbers to call. Default is "callout_list" |
| **codec_list** | Outbound codec list to offer. Default PCMA,PCMU,OPUS |
| **calling_strategy** | sequential or random |
| **action_on_anwser** | What to do when to call connects. Default is "echo()" |
| **transfer_on_answer** | Or transfer to this extension. Default is 8888 |
| **finish_on** | When to end the campaign. 
     -1: When all numbers in the destination_list have been called
      0: Never
      n: After making n calls |


## Gaussian Distribution
Enable Gaussian distribution? If so, you need to provide the "mean" and the standard deviation. If Gaussian distrib is enabled, call_max_duration, call_min_duration and any duration value in the destination_list will be ignored

If Gaussian distribution is diabled, and there is a duration in the destination_list's `duration` field for the numbers, said duration will be used.
If the duration in the destination_list is 0 or NULL, then we will generate a random number between `call_min_duration` and `max_call_duration` and this number will be used as the duration
  
| Parameter     | Description   |
| ------------- |:-------------:|
| **gaussian_distribution** | Enabled 1, disabled 0. Default 1 |
| **gaussian_distribution_mean** | Gaussian distribution mean, default 30 |
| **gaussian_distribution_stdv** | Gaussian standard deviation, default 10
| **call_max_duration** | Max duration to use for Gaussian Distribution calculation. Default 60 |
| **call_min_duration** | Min duration to use for Gaussian Distribution calculation. Default 20 |

