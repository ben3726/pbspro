/*
 * Copyright (C) 1994-2019 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @file	svr_recov.c
 *
 * @brief
 * 		svr_recov.c - contains functions to save server state and recover
 *
 * Included functions are:
 *	svr_recov()
 *	svr_save()
 *	svr_recov_fs()
 *	svr_save_fs()
 *	save_acl()
 *	recov_acl()
 *	sched_recov_fs()
 *	sched_save_fs()
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include "pbs_ifl.h"
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "job.h"
#include "reservation.h"
#include "queue.h"
#include "server.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "log.h"
#include "pbs_share.h"


/* Global Data Items: */

extern struct server server;
extern pbs_list_head svr_queues;
extern attribute_def svr_attr_def[];
extern char	*path_svrdb;
extern char	*path_svrdb_new;
extern char	*path_scheddb;
extern char	*path_scheddb_new;
extern char	*path_priv;
extern time_t	time_now;
extern char	*msg_svdbopen;
extern char	*msg_svdbnosv;
extern char	pbs_recov_filename[];

extern pbs_sched *sched_alloc(char *sched_name);
/**
 * @brief
 *		Recover server information and attributes from server database
 *
 * @par FunctionaliTY:
 *		This function is only called on Server initialization at start up.
 *		Opens and reads from the server database file "serverdb".
 *		First reads in the fixed binary quick save sub structure and then
 *		uses recov_attr() to read in each attribute which has been saved.
 *		Lastly calls acl_recov() to recover the various ACLs which are stored
 *		in their own files.
 *
 * @par	Note:
 *		server structure, extern struct server server, must be preallocated and
 *		all default values should already be set.
 *
 * @see	pbsd_init.c
 *
 * @param[in]	svrfile	-	path of file to be read PBS_HOME/server_priv/serverdb
 *
 * @return	int
 * @retval	0	: On successful recovery and creation of server structure
 * @retval	-1	: On failure to open or read file.
 */

int
svr_recov_fs(char *svrfile)
{
	int i;
	int sdb;
	void recov_acl(attribute *, attribute_def *, char *, char *);

#ifdef WIN32
	/* secure file as perms could be corrupted during windows shutdown */

	fix_perms(svrfile);
#endif

	(void)strcpy(pbs_recov_filename, svrfile);
	sdb = open(svrfile, O_RDONLY, 0);
	if (sdb < 0) {
		log_err(errno, "svr_recov", msg_svdbopen);
		return (-1);
	}
#ifdef WIN32
	setmode(sdb, O_BINARY);
#endif

	/* read in server structure */

	i = read(sdb, (char *)&server.sv_qs, sizeof(struct server_qs));
	if (i != sizeof(struct server_qs)) {
		if (i < 0)
			log_err(errno, "svr_recov", "read of serverdb failed");
		else
			log_err(errno, "svr_recov", "short read of serverdb");
		(void)close(sdb);
		return (-1);
	}

	/* read in server attributes */

	if (recov_attr_fs(sdb, &server, svr_attr_def, server.sv_attr,
		(int)SRV_ATR_LAST, 0) != 0) {
		log_err(errno, "svr_recov", "error on recovering server attr");
		(void)close(sdb);
		return (-1);
	}
	(void)close(sdb);

	/* recover the server various acls  from their own files */

	for (i=0; i<SRV_ATR_LAST; i++) {
		if (server.sv_attr[i].at_type == ATR_TYPE_ACL) {
			recov_acl(&server.sv_attr[i], &svr_attr_def[i],
				PBS_SVRACL, svr_attr_def[i].at_name);
			if (svr_attr_def[i].at_action != NULL_FUNC)
				svr_attr_def[i].at_action(&server.sv_attr[i],
					&server,
					ATR_ACTION_RECOV);
		}
	}

	return (0);
}

/**
 * @brief
 *		Save the state of the server, server quick save sub structure and
 *		optionally the attribututes.
 *
 * @par Functionality:
 *		Saving is done in one of two modes:
 *		Quick - only the "quick save sub structure" is written in place to
 *		the existing "serverdb" file.
 *		Full  - The quick save sub struture is written to a new file, the set
 *		and non-default valued attributes are appended by calling
 *		save_setup(), save_attr(). and finally save_flush().
 *		The newly written file is then renamed to "serverdb".
 *		The various server ACLs are then written to their own files.
 *
 * @param[in]	ps	-	Pointer to struct server
 * @param[in]	mode	-	type of save, either SVR_SAVE_QUICK or SVR_SAVE_FULL
 *
 * @return	int
 * @retval	 0	: Successful save of data.
 * @retval	-1 	: Failure
 *
 * @par Side Effects:
 *		Message will be written to server log on failures.
 *		The server structure element sv_qs.sv_savetm will be updated to the
 *		current time prior to the quick save area being written.
 *
 * @par	MT-save: No - attribute values would need to be protected from updates
 *		while being written to the file.
 */

int
svr_save_fs(struct server *ps, int mode)
{
	int i;
	int sdb;
	int save_acl(attribute *, attribute_def *, char *, char *);
	int		pmode;

#ifdef WIN32
	pmode = _S_IWRITE | _S_IREAD;
	fix_perms2(path_svrdb_new, path_svrdb);
#else
	pmode = 0600;
#endif

	if (mode == SVR_SAVE_QUICK) {
		sdb = open(path_svrdb, O_WRONLY | O_CREAT | O_Sync, pmode);
		if (sdb < 0) {
			log_err(errno, "svr_recov", msg_svdbopen);
			return (-1);
		}
#ifdef WIN32
		secure_file(path_svrdb, "Administrators",
			READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED);
		setmode(sdb, O_BINARY);
#endif
		while ((i=write(sdb, &ps->sv_qs, sizeof(struct server_qs))) !=
			sizeof(struct server_qs)) {
			if ((i == -1) && (errno == EINTR))
				continue;
			log_err(errno, __func__, msg_svdbnosv);
			return (-1);
		}
#ifdef WIN32
		if (_commit(sdb) != 0) {
			log_err(errno, __func__, "flush server db file to disk failed!");
			close(sdb);
			return (-1);
		}
#endif

		(void)close(sdb);

	} else {	/* SVR_SAVE_FULL Save */

		sdb = open(path_svrdb_new, O_WRONLY | O_CREAT | O_Sync, pmode);
		if (sdb < 0) {
			log_err(errno, "svr_recov", msg_svdbopen);
			return (-1);
		}
#ifdef WIN32
		secure_file(path_svrdb_new, "Administrators", READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED);
		setmode(sdb, O_BINARY);
#endif
		ps->sv_qs.sv_savetm = time_now;

		save_setup(sdb);
		if (save_struct((char *)&ps->sv_qs, sizeof(struct server_qs)) != 0) {
			(void)close(sdb);
			return (-1);
		}

		if (save_attr_fs(svr_attr_def, ps->sv_attr, (int)SRV_ATR_LAST) !=0) {
			(void)close(sdb);
			return (-1);
		}

		if (save_flush() != 0) {
			(void)close(sdb);
			return (-1);
		}

#ifdef WIN32
		if (_commit(sdb) != 0) {
			log_err(errno, __func__, "flush server db file to disk failed!");
			close(sdb);
			return (-1);
		}
#endif
		(void)close(sdb);

#ifdef WIN32
		if (MoveFileEx(path_svrdb_new, path_svrdb,
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {

			errno = GetLastError();
			sprintf(log_buffer, "MoveFileEx(%s, %s) failed!",
				path_svrdb_new, path_svrdb);
			log_err(errno, __func__, log_buffer);
		}
#else
		(void)rename(path_svrdb_new, path_svrdb);
#endif

		/* save the server acls to their own files:	*/
		/* 	priv/svracl/(attr name)			*/

		for (i=0; i<SRV_ATR_LAST; i++) {
			if (ps->sv_attr[i].at_type == ATR_TYPE_ACL)
				save_acl(&ps->sv_attr[i], &svr_attr_def[i],
					PBS_SVRACL, svr_attr_def[i].at_name);
		}
	}
	return (0);
}


/**
 * @brief
 *		Save an Access Control List to its file in PBS_HOME.
 *
 * @par Functionality:
 *		The ACL attribute is encode into a comma separated string and written
 *		into a new file named for the object name with ".new" appended.  After
 *		the successful write, the file is renamed without the ".new" suffix.
 *
 * @see	svr_save() above.
 *
 * @param[in]	attr	-	pointer to the ACL attribute
 * @param[in]	pdef	-	pointer to the attribute definition
 * @param[in]	subdir	-	path of subdirectory in which the file is written,
 *			  				e.g. "PBS_HOME/server_priv"
 * @param[in]	name	-	attribute name, used as file name
 *
 * @return	int
 * @retval	 0	: Successful save of data.
 * @retval	-1	: Failure
 *
 * @par MT-safe: maybe
 */

int
save_acl(attribute *attr, attribute_def *pdef, char *subdir, char *name)
{
	int		fds;
	char		filename1[MAXPATHLEN];
	char		filename2[MAXPATHLEN];
	pbs_list_head	head;
	int		i;
	svrattrl	*pentry;
#ifdef WIN32
	int		pmode = _S_IWRITE | _S_IREAD;
	char		*my_id = "save_acl";
#else
	int		pmode = 0600;
#endif

	if ((attr->at_flags & ATR_VFLAG_MODIFY) == 0)
		return (0);  	/* Not modified, don't bother */

	attr->at_flags &= ~ATR_VFLAG_MODIFY;

	(void)strcpy(filename1, path_priv);
	(void)strcat(filename1, subdir);
	(void)strcat(filename1, "/");
	(void)strcat(filename1, name);

	if ((attr->at_flags & ATR_VFLAG_SET) == 0) {

		/* has been unset, delete the file */

		(void)unlink(filename1);
		return (0);
	}

	(void)strcpy(filename2, filename1);
	(void)strcat(filename2, ".new");

#ifdef WIN32
	fix_perms2(filename2, filename1);
#endif

	fds = open(filename2, O_WRONLY|O_CREAT|O_TRUNC|O_Sync, pmode);
	if (fds < 0) {
		log_err(errno, "save_acl", "unable to open acl file");
		return (-1);
	}

#ifdef WIN32
	secure_file(filename2, "Administrators", READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED);
	setmode(fds, O_BINARY);
#endif

	CLEAR_HEAD(head);
	i = pdef->at_encode(attr, &head, pdef->at_name, NULL, ATR_ENCODE_SAVE, NULL);
	if (i < 0) {
		log_err(-1, "save_acl", "unable to encode acl");
		(void)close(fds);
		(void)unlink(filename2);
		return (-1);
	}

	pentry = (svrattrl *)GET_NEXT(head);
	if (pentry != NULL) {
		/* write entry, but without terminating null */
		while ((i = write(fds, pentry->al_value, pentry->al_valln - 1))
			!= pentry->al_valln - 1) {
			if ((i == -1) && (errno == EINTR))
				continue;
			log_err(errno, "save_acl", "wrote incorrect amount");
			(void)close(fds);
			(void)unlink(filename2);
			return (-1);
		}
		(void)free(pentry);
	}

#ifdef WIN32
	if (_commit(fds) != 0) {
		log_err(errno, __func__, "flush server db file to disk failed!");
		close(fds);
		return (-1);
	}
#endif

	(void)close(fds);

#ifdef WIN32
	if (MoveFileEx(filename2, filename1,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
		errno = GetLastError();
		sprintf(log_buffer, "MoveFileEx(%s, %s) failed!",
			filename2, filename1);
		log_err(errno, "save_acl", log_buffer);
	}
#else

	if (rename(filename2, filename1) < 0) {
		log_err(errno, "save_acl", "unable to rename file");
		return (-1);
	}
#endif

	attr->at_flags &= ~ATR_VFLAG_MODIFY;	/* clear modified flag */
	return (0);
}

/**
 * @brief
 *		Recover an Access Control List from its file in PBS_HOME.
 *
 * @par Functionality:
 *		The ACL attribute as a comma separated string and written into a file
 *		named for the acl name.  That string is read and decoded into the
 *		attribute.
 *
 * @see	acl_save() above.
 *
 * @param[in]	pattr	-	pointer to the ACL attribute
 * @param[in]	pdef	-	pointer to the attribute definition
 * @param[in]	subdir	-	path of subdirectory in which the file is written,
 *			  				e.g. "PBS_HOME/server_priv"
 * @param[in]	name	-	attribute name, used as file name
 *
 * @return	int
 * @retval	 0	: Successful save of data.
 * @retval	-1	: Failure
 *
 * @par MT-safe: maybe
 */

void
recov_acl(attribute *pattr, attribute_def *pdef, char *subdir, char *name)
{
	char		*buf;
	int		fds;
	char		filename1[MAXPATHLEN];
	struct stat	sb;
	attribute	tempat;
	int		pmode;

#ifdef WIN32
	pmode = _S_IWRITE | _S_IREAD;
#else
	pmode = 0600;
#endif

	errno = 0;
	(void)strcpy(filename1, path_priv);
	if (subdir != NULL) {
		(void)strcat(filename1, subdir);
		(void)strcat(filename1, "/");
	}
	strcat(filename1, name);

#ifdef WIN32
	fix_perms(filename1);
#endif

	fds = open(filename1, O_RDONLY, pmode);
	if (fds < 0) {
		if (errno != ENOENT) {
			(void)sprintf(log_buffer, "unable to open acl file %s",
				filename1);
			log_err(errno, "recov_acl", log_buffer);
		}
		return;
	}
#ifdef WIN32
	setmode(fds, O_BINARY);
#endif

	if (fstat(fds, &sb) < 0) {
		return;
	}
	if (sb.st_size == 0)
		return;		/* no data */
	buf = malloc((size_t)sb.st_size + 1);	/* 1 extra for added null */
	if (buf == NULL) {
		(void)close(fds);
		return;
	}
	if (read(fds, buf, (unsigned int)sb.st_size) != (int)sb.st_size) {
		log_err(errno, "recov_acl", "unable to read acl file");
		(void)close(fds);
		(void)free(buf);
		return;
	}
	(void)close(fds);
	*(buf + sb.st_size) = '\0';

	clear_attr(&tempat, pdef);

	if (pdef->at_decode(&tempat, pdef->at_name, NULL, buf) < 0) {
		(void)sprintf(log_buffer,  "decode of acl %s failed",
			pdef->at_name);
		log_err(errno, "recov_acl", log_buffer);
	} else if (pdef->at_set(pattr, &tempat, SET) != 0) {
		(void)sprintf(log_buffer, "set of acl %s failed",
			pdef->at_name);
		log_err(errno, "recov_acl", log_buffer);
	}
	pdef->at_free(&tempat);
	(void)free(buf);
	return;
}


static char *schedemsg = "unable to open scheddb";

/**
 * @brief
 *		Recover Scheduler attributes from file scheddb
 *
 * @par FunctionaliTY:
 *		This function is only called on Server initialization at start up.
 *		Opens and reads from the file "scheddb".
 *		As there is no fixed binary quick save for the scheduler structure
 *		which only consists of attributes, uses recov_attr() to read in each
 *		attribute which has been saved.
 *
 * @par	Note:
 *		scheduler structure, extern struct sched scheduler, must be
 *		preallocated and all default values should already be set.
 *
 * @see	pbsd_init.c
 *
 * @param[in]	svrfile	-	path of file to be read PBS_HOME/server_priv/scheddb
 *
 * @return	int
 * @retval	 0 :	On successful recovery and creation of server structure
 * @retval	-1 "	On failutre to open or read file.
 */

int
sched_recov_fs(char *svrfile)
{
	int sdb;
	void recov_acl(attribute *, attribute_def *, char *, char *);

#ifdef WIN32
	/* secure file as perms could be corrupted during windows shutdown */

	fix_perms(svrfile);
#endif

	(void)strcpy(pbs_recov_filename, svrfile);
	sdb = open(svrfile, O_RDONLY, 0);
	if (sdb < 0) {
		if (errno == ENOENT) {
			return (0);	/* no file is ok */
		}else {
			log_err(errno, __func__, schedemsg);
			return (-1);
		}
	}
#ifdef WIN32
	setmode(sdb, O_BINARY);
#endif

	/* read in server attributes */
	/* create default */
	if((dflt_scheduler = sched_alloc(PBS_DFLT_SCHED_NAME))) {
		if (recov_attr_fs(sdb, dflt_scheduler, sched_attr_def, dflt_scheduler->sch_attr,
			(int)SCHED_ATR_LAST, 0) != 0) {
			log_err(errno, __func__, "error on recovering sched attr");
			(void)close(sdb);
			return (-1);
		}
	}
	(void)close(sdb);

	return (0);
}

/**
 * @brief
 *		Save the state of the scheduler structure which consists only of
 *		attributes.
 *
 * @par Functionality:
 *		Saving is done only in Full mode:
 *		Full  - The attributes which are set and non-default are written by
 *		calling save_setup(), save_attr(). and finally save_flush().
 *		The newly written file is then renamed to "scheddb".
 *
 * @param[in]	ps   -	Pointer to struct sched
 * @param[in]	mode -  type of save, only SVR_SAVE_FULL
 *
 * @return	int
 * @retval	 0 :	Successful save of data.
 * @retval	-1 :	Failure
 *
 * @par Side Effects:
 *		Message will be written to server log on failures.
 *
 * @par	MT-save: No - attribute values would need to be protected from updates
 *		while being written to the file.
 */

int
sched_save_fs(struct pbs_sched *ps, int mode)
{
	int sdb;
	int save_acl(attribute *, attribute_def *, char *, char *);
	int		pmode;

#ifdef WIN32
	pmode = _S_IWRITE | _S_IREAD;
	fix_perms2(path_scheddb_new, path_scheddb);
#else
	pmode = 0600;
#endif

	if (mode == SVR_SAVE_QUICK)
		return (-1);	/* invalid mode currently for scheduler */

	/* Since only attributes, we only do a SVR_SAVE_FULL Save */

	sdb = open(path_scheddb_new, O_WRONLY|O_CREAT|O_Sync, pmode);
	if (sdb < 0) {
		log_err(errno, __func__, schedemsg);
		return (-1);
	}
#ifdef WIN32
	secure_file(path_scheddb_new, "Administrators",
		READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED);
	setmode(sdb, O_BINARY);
#endif
	save_setup(sdb);

	if (save_attr_fs(sched_attr_def, ps->sch_attr, (int)SCHED_ATR_LAST) !=0) {
		(void)close(sdb);
		return (-1);
	}

	if (save_flush() != 0) {
		(void)close(sdb);
		return (-1);
	}

#ifdef WIN32
	if (_commit(sdb) != 0) {
		log_err(errno, __func__, "flush server db file to disk failed!");
		close(sdb);
		return (-1);
	}
#endif
	(void)close(sdb);

#ifdef WIN32
	if (MoveFileEx(path_scheddb_new, path_scheddb,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {

		errno = GetLastError();
		sprintf(log_buffer, "MoveFileEx(%s, %s) failed!",
			path_scheddb_new, path_scheddb);
		log_err(errno, __func__, log_buffer);
	}
#else
	(void)rename(path_scheddb_new, path_scheddb);
#endif

	return (0);
}

