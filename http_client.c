#define _BSD_SOURCE
#include <sys/time.h>	// timersub, timercmp
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <curl/curl.h>

// All easy handles are pre-prepared in advance.
// This specifies the total number of requests / easy handles that the application will do in total.
#define NRREQUESTS 32

void print_time_prefix()
{
  struct timeval tv;
  time_t nowtime;
  struct tm *nowtm;
  char tmbuf[64], buf[80];

  gettimeofday(&tv, NULL);
  nowtime = tv.tv_sec;
  nowtm = localtime(&nowtime);
  strftime(tmbuf, sizeof tmbuf, "%Y-%m-%d %H:%M:%S", nowtm);
  snprintf(buf, sizeof buf, "%s.%06u: ", tmbuf, tv.tv_usec);

  static int notfirst = 0;
  static struct timeval last_tv;

  if (!notfirst)
    notfirst = 1;
  else
  {
    static struct timeval mindiff = { 0, 50000 };
    struct timeval diff;
    timersub(&tv, &last_tv, &diff);
    if (timercmp(&diff, &mindiff, >))
    {
      printf("<... %u.%06u seconds ...>\n", diff.tv_sec, diff.tv_usec);
    }
  }
  printf("%s: ", buf);

  last_tv = tv;
}

void add_next_handle(CURLM* multi_handle, CURL* handles[], int* added, int* running)
{
  curl_multi_add_handle(multi_handle, handles[*added]);
  ++*added;
  ++*running;
  print_time_prefix();
  printf("Request #%d    added [now running: %d]\n", *added, *running);
}

void process_results(CURLM* multi_handle, CURL* handles[], int* running)
{
  CURLMsg* msg;
  int msgs_left;
  while ((msg = curl_multi_info_read(multi_handle, &msgs_left)))
  {
    if (msg->msg == CURLMSG_DONE)
    {
      // Find out which handle this message is about.
      int found = 0;
      for (int i = 0; i < NRREQUESTS; ++i)
      {
	if (msg->easy_handle == handles[i])
	{
	  found = i + 1;
	  break;
	}
      }
      if (found)
      {
	print_time_prefix();
	if (msg->data.result == 28)
	{
	  printf("Request    #%d TIMED OUT!", found);
	}
	else if (msg->data.result == 0)
	{
	  printf("Request    #%d finished", found);
	}
	else
	{
	  printf("Request    #%d completed with status %d", found, msg->data.result);
	}
	--*running;
	printf(" [now running: %d]\n", *running);
      }
      else
      {
	printf("Got CURLMSG_DONE for a msg that matches none of our fds!");
      }
    }
  }
}

int main(void)
{
  // Initialize the CURL multi handle.
  CURLM* multi_handle = curl_multi_init();
  curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, 1L);
  curl_multi_setopt(multi_handle, CURLMOPT_MAX_PIPELINE_LENGTH, (long)NRREQUESTS);

  // Custom headers.
  struct curl_slist* headers[NRREQUESTS];
  memset(headers, 0, sizeof headers);
  char header_buf[256];

  // Initialize all CURL easy handles.
  CURL* handles[NRREQUESTS];
  for (int i = 0; i < NRREQUESTS; ++i)
  {
    handles[i] = curl_easy_init();
    curl_easy_setopt(handles[i], CURLOPT_STDERR, stdout);
    curl_easy_setopt(handles[i], CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(handles[i], CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(handles[i], CURLOPT_TIMEOUT, 4L);					// Timeout after 4 seconds.
    curl_easy_setopt(handles[i], CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(handles[i], CURLOPT_URL, "http://localhost:9000/");
    // Construct the headers.
    snprintf(header_buf, sizeof header_buf, "X-Sleep: %d", (i != 8) ? 500 : 5000);	// Server delays reply 0.5 seconds except for request #8, which will be 5 seconds delayed.
    if (i > 0)	// No delay for the first one, which is just to establish that the server supports http pipelining.
      headers[i] = curl_slist_append(headers[i], header_buf);
    snprintf(header_buf, sizeof header_buf, "X-Request: %d", i + 1);			// The requests are numbered 1 through NRREQUESTS.
    headers[i] = curl_slist_append(headers[i], header_buf);
    curl_easy_setopt(handles[i], CURLOPT_HTTPHEADER, headers[i]);
  }

  // The number of actually added easy handles so far. It is (therefore) also used as index
  // to the handles[] array to read the next easy handle to add.
  int added = 0;
  // This variable keeps track of how many easy handles were added (added) minus the number of finished.
  // In other words, the number that is still running.
  int running = 0;

  // Start with adding just one handle - until libcurl saw that it supports pipelining.
  // Otherwise it will create many connections - instead of 1.
  add_next_handle(multi_handle, handles, &added, &running);

  // Brute force let this finish.. it's not really important - just to make sure
  // that libcurl start to do pipelining for this url.
  int still_running;
  do { curl_multi_perform(multi_handle, &still_running); } while (still_running);
  process_results(multi_handle, handles, &running);

  //==========================================================================================
  // THE REAL TEST STARTS HERE

  // Run until nothing is running anymore.
  for (;;)
  {
    // Keep 6 requests in the pipeline, until we run out of easy handles.
    for (int n = still_running; n < 6 && added < NRREQUESTS; ++n)
    {
      // Add the next (already prepared) easy handle.
      add_next_handle(multi_handle, handles, &added, &running);
    }

    // Call curl_multi_perform.
    printf("Running curl_multi_perform() with %d requests in the pipeline.\n", running);
    curl_multi_perform(multi_handle, &still_running);
    printf("still_running = %d\n", still_running);

    // Print debug outout when anything finished, and update 'running'.
    process_results(multi_handle, handles, &running);

    // Exit the main loop when we're done.
    if (running == 0 &&		// all done
	added == NRREQUESTS)	// nothing else to add
      break;

    // Obtain the next timeout by calling curl_multi_timeout().
    struct timeval timeout = { 1, 0 };
    long curl_timeo = -1;
    curl_multi_timeout(multi_handle, &curl_timeo);
    if (curl_timeo >= 0)
    {
      timeout.tv_sec = curl_timeo / 1000;
      if (timeout.tv_sec > 1)
	timeout.tv_sec = 1;
      else
	timeout.tv_usec = (curl_timeo % 1000) * 1000;
    }

    // Obtain the other parameters needed for select() by calling curl_multi_fdset().
    fd_set fdread;
    fd_set fdwrite;
    fd_set fdexcep;
    FD_ZERO(&fdread);
    FD_ZERO(&fdwrite);
    FD_ZERO(&fdexcep);
    int maxfd = -1;
    curl_multi_fdset(multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);

    // Do the select() call.
    int rc;
    do
    {
      printf("select(%d, ..., %d s + %d us) = ", maxfd + 1, timeout.tv_sec, timeout.tv_usec);
      rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);
      printf("%d\n", rc);
    } while (rc == -1 && errno == EAGAIN);
    if (rc == -1)
    {
      printf("select returned an error\n");
      break;
    }

  } // Main loop.

  //==========================================================================================
  // Clean up.

  // Free the headers.
  for (int i = 0; i < NRREQUESTS; ++i)
    curl_slist_free_all(headers[i]);

  // Free the CURL handles.
  for (int i = 0; i < NRREQUESTS; ++i)
    curl_easy_cleanup(handles[i]);

  // Clean up the multi handle.
  curl_multi_cleanup(multi_handle);

  return 0;
}
