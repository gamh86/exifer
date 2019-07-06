#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "exif.h"
#include "logging.h"

#define DATA_COL			"\e[38;5;88m"

static struct sigaction new_act, old_act;
static sigjmp_buf				__sigsegv__;

static void
sigsegv_handler(int signo)
{
	if (signo != SIGSEGV)
		return;
	else
		siglongjmp(__sigsegv__, 1);
}

static int
set_signal_handler(void)
{
	memset(&new_act, 0, sizeof(new_act));
	memset(&old_act, 0, sizeof(old_act));
	sigemptyset(&new_act.sa_mask);
	new_act.sa_handler = sigsegv_handler;
	new_act.sa_flags = 0;
	if (sigaction(SIGSEGV, &new_act, &old_act) < 0)
		return -1;
	else
		return 0;
}

static void
restore_signal_handler(void)
{
	if (sigaction(SIGSEGV, &old_act, NULL) < 0)
		log_error("Failed to restore old signal handler for SIGSEGV");

	return;
}

static unsigned char
random_byte(void)
{
	int						fd;
	unsigned char	c;

	fd = open("/dev/urandom", O_RDONLY);
	read(fd, &c, 1);
	close(fd);
	return c;
}

static void
wipe_data(file_t *file, datum_t *datum)
{
	unsigned char		*p = NULL, *s = NULL, *e = NULL;
	int							i;

	p = s = (unsigned char *)datum->data_start;
	e = (unsigned char *)datum->data_end;
	assert(p < e);

	for (i = 0; i < 8; ++i)
	  {
			unsigned char			r;

			r = random_byte();
			while (p != e)
				*p++ = r;
			p = s;
	  }

	p = s;
	while (p < e)
		*p++ = 0;

	p = s = e = NULL;
	return;
}

static void *
exif_start(file_t *file)
{
	unsigned char						*p = NULL;

	assert(file);
	assert(file->map);
	p = (unsigned char *)file->map;
	while (strncmp((char *)"\xff\xe1", (char *)p, 2) != 0 && p < (unsigned char *)file->map_end)
		++p;
	if (p == (unsigned char *)file->map_end)
	  {
			p = file->map;
			while (strncasecmp((char *)"Exif", p, 4) != 0 && p < (unsigned char *)file->map_end)
				++p;
			return (void *)p;
	  }
	else
		return (void *)p;
}

static void *
get_data_offset(file_t *file, datum_t *dptr, char *str, size_t slen, int endian)
{
	unsigned char				*p = NULL, *t = NULL;
	int									i;

	assert(lim > file->map);
	assert(file->new_end <= file->map_end);

	p = (unsigned char *)exif_start(file);
	while (strncmp(str, (char *)p, slen) != 0 && p < (unsigned char *)lim && p < (unsigned char *)file->new_end)
		++p;

	assert(strncmp((char *)"\xff\xe1", (char *)p, 2) != 0);

	if ((void *)p == lim || (void *)p == file->new_end)
		return NULL;

	dptr->tag_p = (void *)p;
	dptr->type_p = (void *)((unsigned char *)p + 2);
	dptr->len_p = (void *)((unsigned char *)p + 4);
	dptr->offset_p = (void *)((unsigned char *)p + 8);

	if (endian)
	  {
			uint32_t 	*offset = NULL;
			uint32_t	*len = NULL;
			uint16_t	*type = NULL;
			uint8_t		*t = NULL;

			offset = (uint32_t *)dptr->offset_p;
			t = (unsigned char *)offset;
			if (*t == 0)
				*offset = htonl(*offset);
			dptr->offset = *offset;

			len = (uint32_t *)dptr->len_p;
			*len = ntohl(*len);
			t = (unsigned char *)len;
			if (*t == 0)
				*len = htonl(*len);
			dptr->len = *len;

			type = (uint16_t *)dptr->type_p;
			*type = ntohs(*type);
			t = (unsigned char *)type;
			if (*t == 0)
				*type = ntohs(*type);
			dptr->type = *type;

			offset = NULL;
			len = NULL;
			type = NULL;
			t = NULL;
	  }
	else
	  {
			uint32_t 	*offset = NULL;
			uint32_t	*len = NULL;
			uint16_t	*type = NULL;
			uint8_t		*t = NULL;

			offset = (uint32_t *)dptr->offset_p;
#ifdef DEBUG
			printf("offset: 0x%x\n", *offset);
#endif
			t = (unsigned char *)offset;
			if (*t == 0)
			  {
#ifdef DEBUG
				fprintf(stderr, "endianness is mixed up ... (value = %u)%s", *offset, _EOL);
#endif
				*offset = ntohl(*offset);
			  }
			dptr->offset = *offset;

			len = (uint32_t *)dptr->len_p;
			t = (unsigned char *)len;
			if (*t == 0)
				*len = ntohl(*len);
			dptr->len = *len;

			type = (uint16_t *)dptr->type_p;
			t = (unsigned char *)type;
			if (*t == 0)
				*type = ntohs(*type);
			dptr->type = *type;

			assert(*offset == dptr->offset);
			offset = NULL;
			len = NULL;
			type = NULL;
			t = NULL;
	  }

	if (dptr->offset >= (lim - file->map) || dptr->offset >= (file->map_end - file->map))
	  {
			memset(dptr, 0, sizeof(*dptr));
			return NULL;
	  }
	else
	  {
			dptr->data_start = (void *)((unsigned char *)file->map + dptr->offset + EXIF_DATA_OFFSET);
			unsigned char *p = (unsigned char *)dptr->data_start;
			while (*p != 0)
				++p;
			dptr->data_end = (void *)p;
			p = NULL;
			return (void *)dptr;
	  }
}

int
get_date_time(file_t *file, int endian)
{
	char							*date_time = NULL;
	int								count = 0, i;
	datum_t						datum;
	void							*p = NULL;

	set_signal_handler();
	if (sigsetjmp(__sigsegv__, 0) != 0)
	  {
			log_error("Caught segmentation fault - exiting");
			exit(EXIT_FAILURE);
	  }

	p = get_data_offset(file, &datum, endian ? (char *)"\x90\x02" : (char *)"\x02\x90", 2, endian);
	if (p && isdigit(*((char *)datum.data_start)))
	  {
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Digitised:",
								FLAGS & (WIPE_ALL | WIPE_DATE) ? "\e[9;02m" : "",
								DATA_COL,
								(char *)datum.data_start, _EOL);
			++count;
			if (FLAGS & (WIPE_ALL | WIPE_DATE))
				wipe_data(file, &datum);
	  }
	p = get_data_offset(file, &datum, endian ? (char *)"\x90\x03" : (char *)"\x03\x90", 2, endian);
	if (p && isdigit(*((char *)datum.data_start)))
	  {
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Original:",
							FLAGS & (WIPE_ALL | WIPE_DATE) ? "\e[9;02m" : "",
							DATA_COL,
							(char *)datum.data_start, _EOL);
			++count;
			if (FLAGS & (WIPE_ALL | WIPE_DATE))
				wipe_data(file, &datum);
	  }
	p = get_data_offset(file, &datum, endian ? (char *)"\x90\x04" : (char *)"\x04\x90", 2, endian);
	if (p && isdigit(*((char *)datum.data_start)))
	  {
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Created:",
							FLAGS & (WIPE_ALL | WIPE_DATE) ? "\e[9;02m" : "",
							DATA_COL,
							(char *)datum.data_start, _EOL);
			++count;
			if (FLAGS & (WIPE_ALL | WIPE_DATE))
					wipe_data(file, &datum);
	  }
	p = get_data_offset(file, &datum, endian ? (char *)"\x01\x32" : (char *)"\x32\x01", 2, endian);
	if (p && isdigit(*((char *)datum.data_start)))
	  {
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Modified:",
							FLAGS & (WIPE_ALL | WIPE_DATE) ? "\e[9;02m" : "",
							DATA_COL,
							(char *)datum.data_start, _EOL);
			++count;
			if (FLAGS & (WIPE_ALL | WIPE_DATE))
				wipe_data(file, &datum);
	  }

	restore_signal_handler();

	return count;
}

int
get_latitude(file_t *file, int endian)
{
	double				*deg_num = NULL, *deg_denom = NULL;
	int						count;

	//deg_num = (double *)get_data_offset(data, data_end, endian ? (char *)"\x00\x02" : (char *)"\x02\x00", 2, endian);

	count = 0;
	return count;
}

int
get_make_model(file_t *file, int endian)
{
	void					*p = NULL;
	int						count = 0;
	datum_t				datum;

	set_signal_handler();
	if (sigsetjmp(__sigsegv__, 0) != 0)
	  {
			log_error("Caught segmentation fault - exiting");
			exit(EXIT_FAILURE);
	  }

	memset(&datum, 0, sizeof(datum));

	p = get_data_offset(file, &datum, endian ? (char *)"\x01\x0f" : (char *)"\x0f\x01", 2, endian);
	if (p && isalpha(*((char *)datum.data_start)))
	  {
			++count;
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Manufacturer:",
							FLAGS & (WIPE_ALL | WIPE_DEVICE) ? "\e[9;02m" : "",
							DATA_COL,
							(char *)datum.data_start, _EOL);
			if (FLAGS & (WIPE_ALL | WIPE_DEVICE))
				wipe_data(file, &datum);
	  }

	p = get_data_offset(file, &datum, endian ? (char *)"\x01\x10" : (char *)"\x10\x01", 2, endian);
	if (p && isalpha(*((char *)datum.data_start)))
	  {
			++count;
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Model:",
							FLAGS & (WIPE_ALL | WIPE_DEVICE) ? "\e[9;02m" : "",
							DATA_COL,
							(char *)datum.data_start, _EOL);
			if (FLAGS & (WIPE_ALL | WIPE_DEVICE))
				wipe_data(file, &datum);
	  }

	p = get_data_offset(file, &datum, endian ? (char *)"\x01\x31" : (char *)"\x31\x01", 2, endian);
	if (p && (isalpha(*((char *)datum.data_start)) || isdigit(*((char *)datum.data_start))))
	  {
			++count;
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Software:",
							FLAGS & (WIPE_ALL | WIPE_DEVICE) ? "\e[9;02m" : "",
							DATA_COL,
							(char *)datum.data_start, _EOL);
			if (FLAGS & (WIPE_ALL | WIPE_DEVICE))
				wipe_data(file, &datum);
	  }

	p = get_data_offset(file, &datum, endian ? (char *)"\x92\x7c" : (char *)"\x7c\x92", 2, endian);
	if (p && (isalpha(*((char *)datum.data_start)) || isdigit(*((char *)datum.data_start))))
	  {
			++count;
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Makernote:",
							FLAGS & (WIPE_ALL | WIPE_DEVICE) ? "\e[9;02m" : "",
							DATA_COL,
							(char *)datum.data_start, _EOL);
			if (FLAGS & (WIPE_ALL | WIPE_DEVICE))
				wipe_data(file, &datum);
	  }

	restore_signal_handler();

	return count;
}

int
get_miscellaneous_data(file_t *file, int endian)
{
	datum_t		datum;
	void			*p = NULL;
	int				count;

	set_signal_handler();
	if (sigsetjmp(__sigsegv__, 0) != 0)
	{
		log_error("Caught segmentation fault - exiting!");
		exit(EXIT_FAILURE);
	}

	count = 0;
	memset(&datum, 0, sizeof(datum));

	/* Get image description */
	p = get_data_offset(file, &datum, endian ? (char *)"\x01\x0e" : (char *)"\x0e\x01", 2, endian);
	if (p && isascii(*((char *)datum.data_start)))
	{
		++count;
		printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Image Description:",
					FLAGS & (WIPE_ALL | WIPE_MISC) ? "\e[9;02m" : "",
					DATA_COL,
					(char *)datum.data_start, _EOL);

		if (FLAGS & (WIPE_ALL | WIPE_MISC))
			wipe_data(file, &datum);
	}

	/* Get comments */
	p = get_data_offset(file, &datum, endian ? (char *)"\x90\x86" : (char *)"\x86\x90", 2, endian);
	if (p && isascii(*((char *)datum.data_start)))
	  {
			++count;
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Comment:",
						FLAGS & (WIPE_ALL | WIPE_COMMENT) ? "\e[9;02m" : "",
						DATA_COL,
						(char *)datum.data_start, _EOL);
			if (FLAGS & (WIPE_ALL | WIPE_COMMENT))
				wipe_data(file, &datum);
	  }
	memset(&datum, 0, sizeof(datum));
	p = get_data_offset(file, &datum, endian ? (char *)"\x92\x86" : (char *)"\x92\x86", 2, endian);
	if (p && isascii(*((char *)datum.data_start)))
	{
		++count;
		printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Comment:",
						FLAGS & (WIPE_ALL | WIPE_COMMENT) ? "\e[9;02m" : "",
						DATA_COL,
						(char *)datum.data_start, _EOL);
		if (FLAGS & (WIPE_ALL | WIPE_COMMENT))
			wipe_data(file, &datum);
	}

	/* Get unique image ID */
	p = get_data_offset(file, &datum, endian ? (char *)"\xa4\x20" : (char *)"\x20\xa4", 2, endian);
	if (p && isascii(*((char *)datum.data_start)))
	  {
			++count;
			printf("%*s %s%s%s\e[m%s", OUT_WIDTH, "Unique ID:",
							FLAGS & (WIPE_ALL | WIPE_UID) ? "\e[9;02m" : "",
							DATA_COL,
							(char *)datum.data_start, _EOL);
			if (FLAGS & (WIPE_ALL | WIPE_UID))
					wipe_data(file, &datum);
	  }

	restore_signal_handler();

	return count;
}

int
get_test(file_t *file, int endian)
{
	datum_t			datum;
	void				*p = NULL;

	set_signal_handler();
	if (sigsetjmp(__sigsegv__, 0) != 0)
	  {
			log_error("Caught segmentation fault - exiting!");
			exit(EXIT_FAILURE);
	  }

	memset(&datum, 0, sizeof(datum));
	p = get_data_offset(file, &datum, endian ? (char *)"\x01\x0e" : (char *)"\x0e\x01", 2, endian);

	char		*q = (char *)datum.data_start;
	int		i;

	fprintf(stdout, "%s\n", q);

	restore_signal_handler();
	return 1;
}

void *
get_limit(file_t *file)
{
	unsigned char			*p = NULL;

	p = (unsigned char *)exif_start(file);
	while (strncmp((char *)"\xff\xd8", (char *)p, 2) != 0 && p < (unsigned char *)file->map_end)
	  {
			++p;
	  }
	if (p == (unsigned char *)file->map_end)
		return (file->map + 0x800);
	else
		return (void *)p;
}
