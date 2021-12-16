/*
dscd.c

DSC PC5401 Data Interface Module Daemon

Copyright 2006 Richard Vienneau, Dinamikos Technology Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/


#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>
#include "sqlite3.h"
#include <sys/sysinfo.h>
#include <linux/kernel.h>       /* for struct sysinfo */


/* #define DSC_DEBUG */

#define CLIENT_CONNECTIONS 10
#define CLIENT_BUFFER_SIZE 256
#define SERIAL_BUFFER_SIZE 256
#define GENERAL_BUFFER_SIZE 256
#define CONFIG_LINE_LENGTH 128
#define CONFIG_PARAM_LENGTH 64
#define CONFIG_MAX_ZONES 64

#define VERSION "1.7"

#define TABLE_CREATE_SQL "CREATE TABLE events(time INTEGER, type INTEGER, zoneName TEXT, zone integer); \
CREATE TABLE eventTypes(type PRIMARY KEY, name TEXT); \
INSERT INTO eventTypes (type, name) VALUES (1, \"DCSD Start\");\
INSERT INTO eventTypes (type, name) VALUES (2, \"Zone Open\");\
INSERT INTO eventTypes (type, name) VALUES (3, \"Zone Closed\");\
INSERT INTO eventTypes (type, name) VALUES (4, \"Exit Delay\");\
INSERT INTO eventTypes (type, name) VALUES (5, \"Entry Delay\");\
INSERT INTO eventTypes (type, name) VALUES (6, \"Armed\");\
INSERT INTO eventTypes (type, name) VALUES (7, \"Disarmed\");\
INSERT INTO eventTypes (type, name) VALUES (8, \"Special Open\");\
INSERT INTO eventTypes (type, name) VALUES (9, \"Panic\");\
INSERT INTO eventTypes (type, name) VALUES (10, \"Alarm\");\
INSERT INTO eventTypes (type, name) VALUES (11, \"User Open\");" 

#define DATABASE_NAME "/dscd_db"

#define EVENT_DSCD_START 		1
#define EVENT_ZONE_OPEN  		2
#define EVENT_ZONE_CLOSED 		3
#define EVENT_EXIT_DELAY 		4
#define EVENT_ENTRY_DELAY 		5
#define EVENT_ARMED		 		6
#define EVENT_DISARMED	 		7
#define EVENT_SPECIAL_OPEN 		8
#define EVENT_PANIC 		    9
#define EVENT_ALARM 		    10
#define EVENT_USER_OPEN        11

#define MOTION_DELAY 60 //Record motion every minute

struct client
{
	int socket;
	int buffer_length;
	char buffer[CLIENT_BUFFER_SIZE];
} *client;

int client_count;

int serial_fd;
int serial_length;
char pipe_buffer[SERIAL_BUFFER_SIZE];
char serial_buffer[SERIAL_BUFFER_SIZE];
sqlite3 *db;

char config_serial_port[CONFIG_PARAM_LENGTH];
char config_listen_port[CONFIG_PARAM_LENGTH];
char config_access_code[CONFIG_PARAM_LENGTH];
char config_multiple_partitions[CONFIG_PARAM_LENGTH];
char config_client_events[CONFIG_PARAM_LENGTH];
char config_notify_email[CONFIG_PARAM_LENGTH];
char config_zone[CONFIG_MAX_ZONES + 1][CONFIG_PARAM_LENGTH];
char sql[256];


void process_serial_data(char *buffer, int length);
void process_serial_message(char *buffer, int length);
void process_serial_command(char *buffer, int length);
void process_client_data(int client_index);
void process_client_message(char *buffer, int length);
void send_serial_command(char *buffer, int length);
long int zone_number(char *zone);
int read_configuration(void);
int add_database_event(sqlite3 *db, int type, int zone);
long get_uptime();

int main (int argc, char **argv)
{
	pid_t pid, sid;
	int result;
	struct termios serial_attr;
	fd_set fd_list;
	fd_set fd_test;
	int socket_listen;
	int socket_client;
	struct sockaddr_in socket_address;
	int flag;
	int length;
	int index;
	int index2;
	char *sqlError;

	openlog("dscd", LOG_PID | LOG_NDELAY | LOG_CONS, LOG_USER);
	syslog(LOG_NOTICE, "DSC Daemon Starting...");

#if 1
	/* fork a process */
	pid = fork();
	if (pid < 0)
	{
		syslog(LOG_NOTICE, "DSC Daemon Failed to fork");
		exit(EXIT_FAILURE);
	}

	/* exit parent process */
	if (pid > 0)
	{
		printf("%d", pid);
		exit(EXIT_SUCCESS);
	}

	/* create SID for the child process */
	sid = setsid();
	if (sid < 0)
	{
		syslog(LOG_NOTICE, "DSC Daemon Failed to set session ID");
		exit(EXIT_SUCCESS);
	}

	/* close standard file descriptors */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
#endif

	/* retrieve configuration */
	result = read_configuration();
	if (result)
	{
		syslog(LOG_NOTICE, "DSC Daemon Failed to process config file /etc/dscd.conf");
		exit(EXIT_FAILURE);
	}

	/* open serial port */
	serial_fd = open(config_serial_port, O_RDWR | O_NOCTTY | O_NDELAY);
	if (serial_fd < 0)
	{
		syslog(LOG_NOTICE, "DSC Daemon Failed to open serial port");
		exit(EXIT_FAILURE);
	}

	/* configure serial port */
	result = tcgetattr(serial_fd, &serial_attr);
	if (result)
	{
		syslog(LOG_NOTICE, "DSC Daemon Failed to get serial port attributes");
		exit(EXIT_FAILURE);
	}

	serial_attr.c_iflag = IGNBRK | IGNPAR;
	serial_attr.c_oflag = ONLRET;
	serial_attr.c_lflag = 0;
	serial_attr.c_cflag = CLOCAL | B9600 | CS8 | CREAD;
	
	result = tcsetattr(serial_fd, TCSADRAIN, &serial_attr);
	if (result)
	{
		syslog(LOG_NOTICE, "DSC Daemon Failed to set serial port attributes");
		exit(EXIT_FAILURE);
	}

	flag = 1;
	ioctl(serial_fd, FIONBIO, &flag);

	//Open Database
	result = sqlite3_open(DATABASE_NAME, &db);
	if(result)
	{
		fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
	    sqlite3_close(db);
	}

	//Create table (Might already exist)
	result = sqlite3_exec(db, TABLE_CREATE_SQL, NULL, NULL, &sqlError);
	if(result)
	{
		fprintf(stderr, "SQL Error: %s\n", sqlite3_errmsg(db));
		fprintf(stderr, "SQL Error: %s\n", sqlError);
		sqlite3_free(sqlError);
	}

	syslog(LOG_NOTICE, "DSC Daemon Version %s Started for %s", VERSION, config_serial_port);

	//Add Event type start to database
	add_database_event(db, EVENT_DSCD_START, 0);

	/* allocate memory */
	client = malloc(CLIENT_CONNECTIONS * sizeof(struct client));
	if (client == NULL)
	{
		syslog(LOG_NOTICE, "DSC Daemon Failed allocate memory");
		exit(EXIT_FAILURE);
	}

	/* setup listen socket */
	socket_listen = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_listen < 0)
	{
		syslog(LOG_NOTICE, "DSC Daemon Failed to create socket");
		exit(EXIT_FAILURE);
	}

	bzero(&socket_address, sizeof(socket_address));
	socket_address.sin_family = AF_INET;
	socket_address.sin_addr.s_addr = INADDR_ANY;
	socket_address.sin_port = htons(strtol(config_listen_port, NULL, 10));
	syslog(LOG_NOTICE, "DSC Daemon Port: %s", config_listen_port);

	result = bind(socket_listen, (struct sockaddr *)&socket_address, sizeof(socket_address));
	if (result < 0)
	{
		syslog(LOG_NOTICE, "DSC Daemon Failed to bind socket");
		exit(EXIT_FAILURE);
	}

	listen(socket_listen, 5);

	flag = 1;
	ioctl(socket_listen, FIONBIO, &flag);

	FD_ZERO(&fd_list);
	FD_SET(serial_fd, &fd_list);
	FD_SET(socket_listen, &fd_list);

	/* clear serial buffer */
	serial_length = 0;

	/* clear client count */
	client_count = 0;

	while (1)
	{
		fd_test = fd_list;
		result = select(FD_SETSIZE, &fd_test, 0, 0, 0);
		if (result < 0)
		{
			syslog(LOG_NOTICE, "DSC Daemon Failed on select, resuming");
			continue;
		}

		/* serial port */
		if (FD_ISSET(serial_fd, &fd_test))
		{
			length = read(serial_fd, pipe_buffer, sizeof(pipe_buffer));
			process_serial_data(pipe_buffer, length);
		}

		/* listening socket */
		if (FD_ISSET(socket_listen, &fd_test))
		{
			if (client_count >= CLIENT_CONNECTIONS)
				continue;

			length = sizeof(socket_address);
			socket_client = accept(socket_listen,
					(struct sockaddr *)&socket_address,
					(unsigned int *)&length);
			if (socket_client < 0)
				continue;

			syslog(LOG_NOTICE, "Client connect %d", socket_client);

			flag = 1;
			ioctl(socket_client, FIONBIO, &flag);

			FD_SET(socket_client, &fd_list);

			client[client_count].socket = socket_client;
			client[client_count].buffer_length = 0;
			client_count++;
		}

		/* client sockets */
		for (index = 0; index < client_count; index++)
		{
			if (FD_ISSET(client[index].socket, &fd_test))
			{
				length = read(client[index].socket,
						client[index].buffer + client[index].buffer_length,
						CLIENT_BUFFER_SIZE - client[index].buffer_length - 1);

				if ((length < 0) && (errno == EWOULDBLOCK))
					continue;

				if (length <= 0)
				{
					syslog(LOG_NOTICE, "Client disconnect %d",
							client[index].socket);

					/* close client */
					close(client[index].socket);
					FD_CLR(client[index].socket, &fd_list);
					client[index].socket = 0;
					client_count--;

					/* shift other clients down */
					for (index2 = index; index < client_count; index++)
						client[index2] = client[index2 + 1];

					index--;
					continue;
				}

				client[index].buffer_length += length;

				process_client_data(index);
			}
		}
	}
}

void process_serial_data(char *buffer, int length)
{
	int index;
	int start;

	/* append data to serial buffer */
	memcpy(&serial_buffer[serial_length], buffer, length);
	serial_length += length;

	/* check for message terminators */
	if ((serial_buffer[serial_length - 1] == 0x0a) &&
		(serial_buffer[serial_length - 2] == 0x0d))
	{
		start = 0;
		for (index = 0; index < serial_length; index++)
		{
			if ((serial_buffer[index] == 0x0a) &&
				(serial_buffer[index - 1] == 0x0d))
			{
				process_serial_message(&serial_buffer[start], index - start + 1);
				start = index + 1;
			}
		}

		serial_length = 0;
	}
}

void process_serial_message(char *buffer, int length)
{
	int index;
	int checksum;
	unsigned char upper_nibble;
	unsigned char lower_nibble;

	/* verify checksum */
	checksum = 0;
	for (index = 0; index < length - 4; index++)
		checksum += buffer[index];

	checksum &= 0x000000ff;

	upper_nibble = checksum >> 4;
	if (upper_nibble < 0x0a)
		upper_nibble += 0x30;
	else
		upper_nibble += 0x37;

	lower_nibble = checksum  & 0x0f;
	if (lower_nibble < 0x0a)
		lower_nibble += 0x30;
	else
		lower_nibble += 0x37;

#ifdef DSC_DEBUG
	for (index = 0; index < length; index++)
		syslog(LOG_NOTICE, "Data: %c", buffer[index]);

	syslog(LOG_NOTICE, "Checksum Received: %c,%c",
			buffer[length - 4],
			buffer[length - 3]);
	syslog(LOG_NOTICE, "Checksum Calculated: %c,%c", upper_nibble, lower_nibble);
#endif

	if ((upper_nibble == buffer[length - 4]) &&
		(lower_nibble == buffer[length - 3]))
		process_serial_command(buffer, length);
	else
		syslog(LOG_NOTICE, "DSC Daemon Checksum error");
}

void process_serial_command(char *buffer, int length)
{
	char command_string[3 + 1];
	char data[GENERAL_BUFFER_SIZE];
	long int command;
	char message[GENERAL_BUFFER_SIZE];
	char client_message[GENERAL_BUFFER_SIZE];
	char partition;
	long int zone;
	char thermostat;
	long int temperature;
	char mode = 0;
	int index;
	FILE *mail_fd;
	static unsigned int lastMotion;
	unsigned int upTime;

	/* convert command string to numeric */
	memcpy(command_string, buffer, 3);
	command_string[3] = 0;
	command = strtol(command_string, NULL, 10);

	/* save command data */
	if ((length > 7) && (length < 32))
	{
		memcpy(data, buffer + 3, length - 7);
		data[length - 7] = 0;
	}
	else
		data[0] = 0;

	/* reset partition and zone name */
	partition = '0';
	zone = 0;

	/* generate message */
	switch (command)
	{
		case 500:
			strcpy(message, "Command Acknowledge");
			break;
		case 501:
			strcpy(message, "Command Error");
			break;
		case 502:
			sprintf(message, "System Error: %s", data);
			break;
		case 550:
			sprintf(message, "Time/Date Broadcast: %s", data);
			break;
		case 560:
			strcpy(message, "Ring Detected");
			break;
		case 562:
			thermostat = data[0];
			temperature = zone_number(&data[1]);
			strcpy(message, "Temperature");
			break;
		case 601:
			partition = data[0];
			zone = zone_number(&data[1]);
			strcpy(message, "Zone Alarm");
			break;
		case 602:
			partition = data[0];
			zone = zone_number(&data[1]);
			strcpy(message, "Zone Alarm Restore");
			break;
		case 603:
			partition = data[0];
			zone = zone_number(&data[1]);
			strcpy(message, "Zone Tamper");
			break;
		case 604:
			partition = data[0];
			zone = zone_number(&data[1]);
			strcpy(message, "Zone Tamper Restore");
			break;
		case 605:
			zone = zone_number(data);
			strcpy(message, "Zone Fault");
			break;
		case 606:
			zone = zone_number(data);
			strcpy(message, "Zone Fault Restore");
			break;
		case 609:
			zone = zone_number(data);
			strcpy(message, "Zone Open");

			//We only want to add motion detect every minute
    		if (strstr(config_zone[zone], "Motion") != NULL)  //Found ?
			{
    			upTime = get_uptime(); //Get current uptime
    			if (upTime > (lastMotion + MOTION_DELAY))
    			{
    				//Only add entry if a certain amount of time has passed
    				add_database_event(db, EVENT_ZONE_OPEN, zone);
    				lastMotion = upTime;
    			}
			}
    		else
    		{
    			add_database_event(db, EVENT_ZONE_OPEN, zone);
    		}
			break;
		case 610:
			zone = zone_number(data);
			strcpy(message, "Zone Restore");

			//Do not add Zone Restore messages for Motion detector zones
			if (strstr(config_zone[zone], "Motion") == NULL)
			{
				add_database_event(db, EVENT_ZONE_CLOSED, zone);
			}
			break;
		case 620:
			strcpy(message, "Duress Alarm");
			break;
		case 621:
			strcpy(message, "Panic Alarm Fire");
			break;
		case 622:
			strcpy(message, "Panic Alarm Fire Restore");
			break;
		case 623:
			strcpy(message, "Panic Alarm Auxiliary");
			break;
		case 624:
			strcpy(message, "Panic Alarm Auxiliary Restore");
			break;
		case 625:
			strcpy(message, "Panic Alarm Police");
			add_database_event(db, EVENT_PANIC, zone);
			break;
		case 626:
			strcpy(message, "Panic Alarm Police Restore");
			break;
		case 631:
			strcpy(message, "Smoke Alarm");
			break;
		case 632:
			strcpy(message, "Smoke Alarm Restore");
			break;
		case 650:
			partition = data[0];
			strcpy(message, "Partition Ready");
			add_database_event(db, EVENT_DISARMED, 0);
			break;
		case 651:
			partition = data[0];
			strcpy(message, "Partition Not Ready");
			break;
		case 652:
			partition = data[0];
			if (strlen(data) == 2)
			{
                mode = data[1];
				switch (data[1])
				{
					case '0':
						strcpy(message, "Partition Armed in Away Mode");
						break;
					case '1':
						strcpy(message, "Partition Armed in Stay Mode");
						break;
					case '2':
						strcpy(message, "Partition Armed in Zero Entry Away Mode");
						break;
					case '3':
						strcpy(message, "Partition Armed in Zero Entry Stay Mode");
						break;
				}
			}
			else
			{
				strcpy(message, "Partition Armed");
				add_database_event(db, EVENT_ARMED, 0);
			}
			break;
		case 654:
			partition = data[0];
			strcpy(message, "Partition in Alarm");
			add_database_event(db, EVENT_ALARM, 0);
			break;
		case 655:
			partition = data[0];
			strcpy(message, "Partition Disarmed");
			add_database_event(db, EVENT_DISARMED, 0);
			break;
		case 656:
			partition = data[0];
			strcpy(message, "Exit Delay in Progress");
			add_database_event(db, EVENT_EXIT_DELAY, 0);
			break;
		case 657:
			partition = data[0];
			strcpy(message, "Entry Delay in Progress");
			add_database_event(db, EVENT_ENTRY_DELAY, 0);
			break;
		case 658:
			partition = data[0];
			strcpy(message, "Keypad Lockout");
			break;
		case 670:
			partition = data[0];
			strcpy(message, "Invalid Access Code");
			break;
		case 671:
			partition = data[0];
			strcpy(message, "Function Not Available");
			break;
		case 700:
			partition = data[0];
			strcpy(message, "User Closing");
			break;
		case 701:
			partition = data[0];
			strcpy(message, "Special Closing");
			break;
		case 702:
			partition = data[0];
			strcpy(message, "Partial Closing");
			break;
		case 750:
			partition = data[0];
			strcpy(message, "User Opening");
			add_database_event(db, EVENT_USER_OPEN, 0);
			break;
		case 751:
			partition = data[0];
			strcpy(message, "Special Opening");
			add_database_event(db, EVENT_SPECIAL_OPEN, 0);
			break;
		case 800:
			strcpy(message, "Panel Battery Trouble");
			break;
		case 801:
			strcpy(message, "Panel Battery Trouble Restore");
			break;
		case 802:
			strcpy(message, "Panel AC Trouble");
			break;
		case 803:
			strcpy(message, "Panel AC Restore");
			break;
		case 806:
			strcpy(message, "System Bell Trouble");
			break;
		case 807:
			strcpy(message, "System Bell Trouble Restore");
			break;
		case 810:
		case 812:
			strcpy(message, "Telephone Line Trouble");
			break;
		case 811:
		case 813:
			strcpy(message, "Telephone Line Trouble Restore");
			break;
		case 814:
			strcpy(message, "Failure To Communicate Trouble");
			break;
		case 816:
			strcpy(message, "Panel Event Buffer Near Full");
			break;
		case 821:
			zone = zone_number(data);
			strcpy(message, "Wireless Device Low Battery");
			break;
		case 822:
			zone = zone_number(data);
			strcpy(message, "Wireless Device Low Battery Restore");
			break;
		case 825:
			zone = zone_number(data);
			strcpy(message, "Wireless Key Low Battery");
			break;
		case 826:
			zone = zone_number(data);
			strcpy(message, "Wireless Key Low Battery Restore");
			break;
		case 827:
			zone = zone_number(data);
			strcpy(message, "Handheld Keypad Low Battery");
			break;
		case 828:
			zone = zone_number(data);
			strcpy(message, "Handheld Keypad Low Battery Restore");
			break;
		case 829:
			strcpy(message, "General System Tamper");
			break;
		case 830:
			strcpy(message, "General System Tamper Restore");
			break;
		case 831:
			strcpy(message, "Home Automation Trouble");
			break;
		case 832:
			strcpy(message, "Home Automation Trouble Restore");
			break;
		case 840:
			partition = data[0];
			strcpy(message, "Trouble Status");
			break;
		case 841:
			partition = data[0];
			strcpy(message, "Trouble Status Restore");
			break;
		case 842:
			strcpy(message, "Fire Trouble Alarm");
			break;
		case 843:
			strcpy(message, "Fire Trouble Alarm Restore");
			break;
		case 900:
			strcpy(message, "Code Required");
			break;
		default:
			sprintf(message, "Unhandled Command: %ld", command);
			break;
	}

	/* syslog */
	if ((partition != '0') && (strcmp(config_multiple_partitions, "On") == 0))
	{
		/* multiple partitions */
		if (zone != 0)
			syslog(LOG_NOTICE, "[%c] %s - %s", partition, message, config_zone[zone]);
		else
			syslog(LOG_NOTICE, "[%c] %s", partition, message);
	}
	else
	{
		/* single partition */
		if (zone != 0)
			syslog(LOG_NOTICE, "%s - %s", message, config_zone[zone]);
		else
			syslog(LOG_NOTICE, "%s", message);
	}

	/* client events */
	if (strcmp(config_client_events, "On") == 0)
	{
		if (command == 652)
			sprintf(client_message, "%ld %c %c\r\n", command, partition, mode);
		else
			sprintf(client_message, "%ld %c %02ld\r\n", command, partition, zone);
		for (index = 0; index < client_count; index++)
		{
			if (client[index].socket)
			{
				if (write(client[index].socket, client_message, strlen(client_message)) != strlen(client_message))
					syslog(LOG_NOTICE, "DSC Daemon client socket write length error");
			}
		}
	}
	else if (command == 500)
	{
		for (index = 0; index < client_count; index++)
		{
			if (client[index].socket)
			{
				if (write(client[index].socket, "OK\r\n", 4) != 4)
					syslog(LOG_NOTICE, "DSC Daemon client socket write length error");
			}
		}
	}

	/* notification email */
	switch(command)
	{
		case 601:
		case 620:
		case 621:
		case 623:
		case 625:
		case 631:
		case 654:
			//Create email file
			system("rm /tmp/email.txt");
			system("echo \"to: o2blom@gmail.com\" > /tmp/email.txt");
			system("echo \"subject: DSCD\" >> /tmp/email.txt");
			system("echo \"from: oblomqvist@ca.rr.com\" >> /tmp/email.txt");
			sprintf(data, "echo \"%s %s\" >> /tmp/email.txt", message, config_zone[zone]);
			system(data);
			system("echo \" \" >> /tmp/email.txt"); //Need newline

			//Send email
			system("sendmail -t -f oblomqvist@ca.rr.com -S smtp-server.ca.rr.com < /tmp/email.txt &");
			break;
	}

	/* send access code to system if requested */
	if (command == 900)
	{
		strcpy(data, "200");
		strcat(data, config_access_code);
		send_serial_command(data, strlen(data));
	}
}

void process_client_data(int client_index)
{
	char *buffer;
	int length;
	int index;
	int start;

	buffer = client[client_index].buffer;
	length = client[client_index].buffer_length;

	if ((buffer[length - 1] == 0x0a) &&
		(buffer[length - 2] == 0x0d))
	{
		start = 0;
		for (index = 0; index < length; index++)
		{
			if ((buffer[index] == 0x0a) &&
				(buffer[index - 1] == 0x0d))
			{
				process_client_message(&buffer[start], index - start + 1);
				start = index + 1;
			}
		}

		client[client_index].buffer_length = 0;
	}
}

void process_client_message(char *buffer, int length)
{
	char serial_command[32];
	int valid_command;

	valid_command = 1;

	buffer[length - 2] = 0;

	/* OUTPUTxy where x is the partion, y is the output */
	if ((memcmp(buffer, "OUTPUT", 6) == 0) &&
		(strlen(buffer) == 8) &&
		(buffer[6] >= '1') &&
		(buffer[6] <= '8') &&
		(buffer[7] >= '1') &&
		(buffer[7] <= '4'))
	{
		sprintf(serial_command, "020%c%c", buffer[6], buffer[7]);
		send_serial_command(serial_command, strlen(serial_command));
	}

	/* ARMAWAYx where x is the partition */
	else if ((memcmp(buffer, "ARMAWAY", 7) == 0) &&
		(strlen(buffer) == 8) &&
		(buffer[7] >= '1') &&
		(buffer[7] <= '8'))
	{
		sprintf(serial_command, "030%c", buffer[7]);
		send_serial_command(serial_command, strlen(serial_command));
	}

	/* ARMSTAYx where x is the partition */
	else if ((memcmp(buffer, "ARMSTAY", 7) == 0) &&
		(strlen(buffer) == 8) &&
		(buffer[7] >= '1') &&
		(buffer[7] <= '8'))
	{
		sprintf(serial_command, "031%c", buffer[7]);
		send_serial_command(serial_command, strlen(serial_command));
	}

	/* ARMNODELAYx where x is the partition */
	else if ((memcmp(buffer, "ARMNODELAY", 10) == 0) &&
		(strlen(buffer) == 11) &&
		(buffer[10] >= '1') &&
		(buffer[10] <= '8'))
	{
		sprintf(serial_command, "032%c", buffer[10]);
		send_serial_command(serial_command, strlen(serial_command));
	}

	/* ARMx with code where x is the partition */
	else if ((memcmp(buffer, "ARM", 3) == 0) &&
		(strlen(buffer) == 4) &&
		(buffer[3] >= '1') &&
		(buffer[3] <= '8'))
	{
		sprintf(serial_command, "033%c", buffer[3]);
		strcat(serial_command, config_access_code);
		send_serial_command(serial_command, strlen(serial_command));
	}

	/* DISARMx where x is the partition */
	else if ((memcmp(buffer, "DISARM", 6) == 0) &&
		(strlen(buffer) == 7) &&
		(buffer[6] >= '1') &&
		(buffer[6] <= '8'))
	{
		sprintf(serial_command, "040%c", buffer[6]);
		strcat(serial_command, config_access_code);
		send_serial_command(serial_command, strlen(serial_command));
	}

	/* PANICx where x is the mode (1=Fire, 2=Auxiliary, 3=Police) */
	else if ((memcmp(buffer, "PANIC", 5) == 0) &&
		(strlen(buffer) == 6) &&
		(buffer[5] >= '1') &&
		(buffer[5] <= '3'))
	{
		sprintf(serial_command, "060%c", buffer[5]);
		send_serial_command(serial_command, strlen(serial_command));
	}

	/* DESCARMx where x is the mode */
	else if ((memcmp(buffer, "DESCARM", 7) == 0) &&
		(strlen(buffer) == 8) &&
		(buffer[7] >= '0') &&
		(buffer[7] <= '1'))
	{
		sprintf(serial_command, "050%c", buffer[7]);
		send_serial_command(serial_command, strlen(serial_command));
	}

	/* TEMPx where x is the mode */
	else if ((memcmp(buffer, "TEMP", 4) == 0) &&
		(strlen(buffer) == 5) &&
		(buffer[4] >= '0') &&
		(buffer[4] <= '1'))
	{
		sprintf(serial_command, "057%c", buffer[4]);
		send_serial_command(serial_command, strlen(serial_command));
	}

	else
		valid_command = 0;

	if (valid_command)
		syslog(LOG_NOTICE, "Client command: %s", buffer);
}

void send_serial_command(char *buffer, int length)
{
	int result;
	int index;
	int checksum;
	unsigned char upper_nibble;
	unsigned char lower_nibble;

	memcpy(pipe_buffer, buffer, length);

	/* generate checksum */
	checksum = 0;
	for (index = 0; index < length; index++)
		checksum += pipe_buffer[index];

	checksum &= 0x000000ff;

	upper_nibble = checksum >> 4;
	if (upper_nibble < 0x0a)
		upper_nibble += 0x30;
	else
		upper_nibble += 0x37;

	lower_nibble = checksum  & 0x0f;
	if (lower_nibble < 0x0a)
		lower_nibble += 0x30;
	else
		lower_nibble += 0x37;

	pipe_buffer[length++] = upper_nibble;
	pipe_buffer[length++] = lower_nibble;

	pipe_buffer[length++] = 0x0d;
	pipe_buffer[length++] = 0x0a;

	result = write(serial_fd, pipe_buffer, length);

	pipe_buffer[length] = 0;
}

long int zone_number(char *zone)
{
	char zone_string[4];
	long int zone_number;

	memcpy(zone_string, zone, 3);
	zone_string[3] = 0;

	zone_number = strtol(zone_string, NULL, 10);

	if ((zone_number > 0) && (zone_number <= CONFIG_MAX_ZONES))
		return zone_number;
	else
		return 0;
}

int read_configuration(void)
{
	FILE *config_fd;
	char buffer[CONFIG_LINE_LENGTH];
	char *ptr;
	char *parameter_ptr;
	char *value_ptr;
	char *config_ptr;
	char zone[16];
	int index;

	/* initialize configuration parameters */
	config_serial_port[0] = 0;
	config_listen_port[0] = 0;
	config_access_code[0] = 0;
	config_multiple_partitions[0] = 0;

	for (index = 1; index <= CONFIG_MAX_ZONES; index++)
		config_zone[index][0] = 0;

	/* open configuration file */
	config_fd = fopen("/etc/dscd.conf", "r");
	if (config_fd < 0)
		return -1;

	/* read lines */
	while (fgets(buffer, CONFIG_LINE_LENGTH, config_fd) != NULL)
	{
		/* null terminate line */
		buffer[CONFIG_LINE_LENGTH - 1] = 0;

		/* ignore comments */
		if (buffer[0] == '#')
			continue;

		/* skip whitespace */
		ptr = buffer;
		while (*ptr == ' ' || *ptr == '\t')
			ptr++;

		/* parameter name */
		parameter_ptr = ptr;
		while (*ptr != ' ' && *ptr != '\t' && *ptr != '\n' && *ptr != 0)
			ptr++;
		*ptr++ = 0;

		/* skip whitespace */
		while (*ptr == ' ' || *ptr == '\t')
			ptr++;

		/* parameter value */
		value_ptr = ptr;
		while (*ptr != '\n' && *ptr != 0)
			ptr++;
		*ptr = 0;

		/* handle normal parameters */
		if (strcmp(parameter_ptr, "SERIAL_PORT") == 0)
			config_ptr = config_serial_port;
		else if (strcmp(parameter_ptr, "LISTEN_PORT") == 0)
			config_ptr = config_listen_port;
		else if (strcmp(parameter_ptr, "ACCESS_CODE") == 0)
			config_ptr = config_access_code;
		else if (strcmp(parameter_ptr, "MULTIPLE_PARTITIONS") == 0)
			config_ptr = config_multiple_partitions;
		else if (strcmp(parameter_ptr, "CLIENT_EVENTS") == 0)
			config_ptr = config_client_events;
		else if (strcmp(parameter_ptr, "NOTIFY_EMAIL") == 0)
			config_ptr = config_notify_email;
		else
			config_ptr = NULL;

		if (config_ptr)
		{
			strncpy(config_ptr, value_ptr, CONFIG_PARAM_LENGTH);
			config_ptr[CONFIG_PARAM_LENGTH - 1] = 0;
		}
		else
		{
			/* handle zone name parameters */
			for (index = 1; index <= CONFIG_MAX_ZONES; index++)
			{
				sprintf(zone, "ZONE_%d", index);
				if (strcmp(parameter_ptr, zone) == 0)
				{
					strncpy(config_zone[index], value_ptr, CONFIG_PARAM_LENGTH);
					config_zone[index][CONFIG_PARAM_LENGTH - 1] = 0;
				}
			}
		}
	}

	fclose(config_fd);

	return 0;
}

int add_database_event(sqlite3 *db, int type, int zone)
{
	int result;
	char *sqlError;
	char zoneName[64];

	//If certain message type we add the Zonename
	if (type == EVENT_ZONE_OPEN || type == EVENT_ZONE_CLOSED)
	{
		strcpy(zoneName, config_zone[zone]);
	}
	else
	{
		sprintf(zoneName, "n/a");
	}

	//Build SQL
	sprintf(sql, "INSERT INTO events VALUES (datetime('now', 'localtime'), %d, '%s', %d);", type, zoneName, zone);

	result = sqlite3_exec(db, sql, NULL, NULL, &sqlError);
	if(result)
	{
		fprintf(stderr, "SQL Error: %s\n", sqlite3_errmsg(db));
		fprintf(stderr, "SQL Error: %s\n", sqlError);
		sqlite3_free(sqlError);
		sqlite3_close(db);
	    return(1);
	}
	return 0;
}

long get_uptime()
{
    struct sysinfo s_info;
    int error;
    error = sysinfo(&s_info);
    if(error != 0)
    {
        printf("code error = %d\n", error);
    }
    return s_info.uptime;
}

