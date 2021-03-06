/*****************************************************************************/
/*                                                                           */
/* Logswan 1.01 (c) by Frederic Cambus 2015                                  */
/* https://github.com/fcambus/logswan                                        */
/*                                                                           */
/* Created:      2015/05/31                                                  */
/* Last Updated: 2015/10/01                                                  */
/*                                                                           */
/* Logswan is released under the BSD 3-Clause license.                       */
/* See LICENSE file for details.                                             */
/*                                                                           */
/*****************************************************************************/

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef HAVE_STRTONUM
#include "../compat/strtonum.h"
#endif

#include "../deps/hll/hll.h"

#include <GeoIP.h>

#include "logswan.h"
#include "definitions.h"
#include "output.h"
#include "parse.h"
#include "results.h"

GeoIP *geoip, *geoipv6;

clock_t begin, end;

char lineBuffer[LINE_MAX_LENGTH];

Results results;
struct date parsedDate;
struct logLine parsedLine;
struct request parsedRequest;

struct sockaddr_in ipv4;
struct sockaddr_in6 ipv6;
int isIPv4, isIPv6;

uint64_t bandwidth;
int statusCode;
int hour;

struct stat logFileSize;
FILE *logFile, *jsonFile;

const char *errstr;

int getoptFlag;

struct HLL uniqueIPv4, uniqueIPv6;

void displayUsage() {
	printf("USAGE : logswan [options] inputfile\n\n" \
	       "Options are :\n\n" \
	       "	-h Display usage\n" \
	       "	-v Display version\n\n");
}

int main (int argc, char *argv[]) {
	char *intputFile;

	hll_init(&uniqueIPv4, 20);
	hll_init(&uniqueIPv6, 20);

	printf("-------------------------------------------------------------------------------\n" \
	       "                    Logswan 1.01 (c) by Frederic Cambus 2015                   \n" \
	       "-------------------------------------------------------------------------------\n\n");

	while ((getoptFlag = getopt(argc, argv, "hv")) != -1) {
		switch(getoptFlag) {
		case 'h':
			displayUsage();
			return EXIT_SUCCESS;

		case 'v':
			printf("%s\n\n", VERSION);
			return EXIT_SUCCESS;
		}
	}

	if (optind < argc) {
		intputFile = argv[optind];
	} else {
		displayUsage();
		return EXIT_SUCCESS;
	}

	argc -= optind;
	argv += optind;

	/* Starting timer */
	begin = clock();

	/* Initializing GeoIP */
	geoip = GeoIP_open("/usr/local/share/GeoIP/GeoIP.dat", GEOIP_MEMORY_CACHE);
	geoipv6 = GeoIP_open("/usr/local/share/GeoIP/GeoIPv6.dat", GEOIP_MEMORY_CACHE);

	/* Get log file size */
	stat(intputFile, &logFileSize);
	results.fileSize = (uint64_t)logFileSize.st_size;

	printf("Processing file : %s\n\n", intputFile);

	if (!(logFile = fopen(intputFile, "r"))) {
		perror("Can't open log file");
		return EXIT_FAILURE;
	}

	/* Create output file */
	int outputLen = strlen(intputFile) + 6;
	char *outputFile = malloc(outputLen);
	snprintf(outputFile, outputLen, "%s%s", intputFile, ".json");

	if (!(jsonFile = fopen(outputFile, "w"))) {
		perror("Can't create output file");
		return EXIT_FAILURE;
	}

	while (fgets(lineBuffer, LINE_MAX_LENGTH, logFile) != NULL) {
		/* Parse and tokenize line */
		parseLine(&parsedLine, lineBuffer);

		/* Detect if remote host is IPv4 or IPv6 */
		if (parsedLine.remoteHost) { /* Do not feed NULL tokens to inet_pton */
			isIPv4 = inet_pton(AF_INET, parsedLine.remoteHost, &(ipv4.sin_addr));
			isIPv6 = inet_pton(AF_INET6, parsedLine.remoteHost, &(ipv6.sin6_addr));
		}

		if (isIPv4 || isIPv6) {
			/* Increment countries array */
			if (geoip && isIPv4) {
				results.countries[GeoIP_id_by_addr(geoip, parsedLine.remoteHost)]++;
			}

			if (geoipv6 && isIPv6) {
				results.countries[GeoIP_id_by_addr_v6(geoipv6, parsedLine.remoteHost)]++;
			}

			/* Unique visitors */
			if (isIPv4) {
				hll_add(&uniqueIPv4, parsedLine.remoteHost, strlen(parsedLine.remoteHost));
			}

			if (isIPv6) {
				hll_add(&uniqueIPv6, parsedLine.remoteHost, strlen(parsedLine.remoteHost));
			}

			/* Hourly distribution */
			parseDate(&parsedDate, parsedLine.date);

			if (parsedDate.hour) {
				hour = strtonum(parsedDate.hour, 0, 23, &errstr);

				if (!errstr) {
					results.hours[hour] ++;
				}
			}

			/* Parse request */
			parseRequest(&parsedRequest, parsedLine.resource);

			if (parsedRequest.method) {
				for (int loop = 0; loop<9; loop++) {
					if (!strcmp(methods[loop], parsedRequest.method)) {
						results.methods[loop] ++;
					}
				}
			}

			if (parsedRequest.protocol) {
				for (int loop = 0; loop<2; loop++) {
					if (!strcmp(protocols[loop], parsedRequest.protocol)) {
						results.protocols[loop] ++;
					}
				}
			}

			/* Count HTTP status codes occurences */
			if (parsedLine.statusCode) {
				statusCode = strtonum(parsedLine.statusCode, 0, STATUS_CODE_MAX-1, &errstr);

				if (!errstr) {
					results.status[statusCode] ++;
				}
			}

			/* Increment bandwidth usage */
			if (parsedLine.objectSize) {
				bandwidth = strtonum(parsedLine.objectSize, 0, INT64_MAX, &errstr);

				if (!errstr) {
					results.bandwidth += bandwidth;
				}
			}

			/* Increment hits counter */
			results.hitsIPv4 += isIPv4;
			results.hitsIPv6 += isIPv6;
			results.hits++;
		} else {
			/* Invalid line */

			results.invalidLines++;
		}

		/* Increment processed lines counter */
		results.processedLines++;
	}

	/* Counting unique visitors */
	results.visitsIPv4 = hll_count(&uniqueIPv4);
	results.visitsIPv6 = hll_count(&uniqueIPv6);
	results.visits = results.visitsIPv4 + results.visitsIPv6;

	/* Stopping timer */
	end = clock();
	results.runtime = (double)(end - begin) / CLOCKS_PER_SEC;

	/* Generate timestamp */
	time_t now = time(NULL);
	strftime(results.timeStamp, 20, "%Y-%m-%d %H:%M:%S", localtime(&now));

	/* Printing results */
	printf("Processed %" PRIu64 " lines in %f seconds\n", results.processedLines, results.runtime);
	fclose(logFile);

	fputs(output(results), jsonFile);
	printf("Created file : %s\n", outputFile);
	fclose(jsonFile);

	GeoIP_delete(geoip);
	GeoIP_delete(geoipv6);

	hll_destroy(&uniqueIPv4);
	hll_destroy(&uniqueIPv6);

	return EXIT_SUCCESS;
}
