/* Copyright (C) 2015-2019, Wazuh Inc.
 * All right reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "shared.h"

static void file_sleep(void);
static int Handle_JQueue(file_queue *fileq, int flags);

/* To translate between month (int) to month (char) */
static const char *(s_month[]) = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void file_sleep(){
#ifndef WIN32
    struct timeval fp_timeout;

    fp_timeout.tv_sec = FQ_TIMEOUT;
    fp_timeout.tv_usec = 0;

    /* Wait for the select timeout */
    select(0, NULL, NULL, NULL, &fp_timeout);

#else
    /* Windows does not like select that way */
    Sleep((FQ_TIMEOUT + 2) * 1000);
#endif
    return;
}

// Initializes queue. Equivalent to initialize every field to 0.
void jqueue_init(file_queue * queue) {
    memset(queue, 0, sizeof(file_queue));
}

/*
 * Open queue with the JSON alerts log file.
 * Returns 0 on success or -1 on error.
 */
int jqueue_open(file_queue * queue, int tail) {
    strncpy(queue->file_name, isChroot() ? ALERTSJSON_DAILY : DEFAULTDIR ALERTSJSON_DAILY, MAX_FQUEUE);

    if (queue->fp) {
        fclose(queue->fp);
    }

    if (queue->fp = fopen(queue->file_name, "r"), !queue->fp) {
        merror(FOPEN_ERROR, queue->file_name, errno, strerror(errno));
        return -1;
    }

    /* Position file queue to end of the file */
    if (tail && fseek(queue->fp, 0, SEEK_END) == -1) {
        merror(FOPEN_ERROR, queue->file_name, errno, strerror(errno));
        fclose(queue->fp);
        queue->fp = NULL;
        return -1;
    }

    /* File inode time */
    if (fstat(fileno(queue->fp), &queue->f_status) < 0) {
        merror(FSTAT_ERROR, queue->file_name, errno, strerror(errno));
        fclose(queue->fp);
        queue->fp = NULL;
        return -1;
    }

    return 0;
}

/*
 * Return next JSON object from the queue, or NULL if it is not available.
 * If no more data is available and the inode has changed, queue is reloaded.
 */
cJSON * jqueue_next(file_queue * queue) {
    struct stat buf;
    char buffer[OS_MAXSTR + 1];
    char *end;
    const char *jsonErrPtr;

    if (!queue->fp && jqueue_open(queue, 1) < 0) {
        return NULL;
    }

    clearerr(queue->fp);

    if (fgets(buffer, OS_MAXSTR + 1, queue->fp)) {
        if (end = strchr(buffer, '\n'), end) {
            *end = '\0';
        }

        return cJSON_ParseWithOpts(buffer, &jsonErrPtr, 0);
    } else {

        if (stat(queue->file_name, &buf) < 0) {
            merror(FSTAT_ERROR, queue->file_name, errno, strerror(errno));
            fclose(queue->fp);
            queue->fp = NULL;
            return NULL;
        }

        // If the inode has changed, reopen and retry to open

        if (buf.st_ino != queue->f_status.st_ino) {
            mdebug2("jqueue_next(): Alert file inode changed. Reloading.");

            if (jqueue_open(queue, 0) < 0) {
                return NULL;
            }

            if (fgets(buffer, OS_MAXSTR + 1, queue->fp)) {
                if (end = strchr(buffer, '\n'), end) {
                    *end = '\0';
                }

                return cJSON_ParseWithOpts(buffer, &jsonErrPtr, 0);
            } else {
                return NULL;
            }
        } else {
            sleep(1);
            return NULL;
        }
    }
}

/* Re Handle the file queue */
static int Handle_JQueue(file_queue *fileq, int flags){
    /* Close if it is open */
    if (!(flags & CRALERT_FP_SET)) {
        if (fileq->fp) {
            fclose(fileq->fp);
            fileq->fp = NULL;
        }

        /* 
            We must be able to open the file, fseek and get the
            time of change from it.
        */
        fileq->fp = fopen(fileq->file_name, "r");
        if (!fileq->fp) {
            /* Queue not available */
            return 0;
        }
    }

    /* Seek to the end of the file */
    if (!(flags & CRALERT_READ_ALL)) {
        if (!fileq->fp) {
            return 0;
        }

        if (fseek(fileq->fp, 0, SEEK_END) < 0) {
            merror(FSEEK_ERROR, fileq->file_name, errno, strerror(errno));
            fclose(fileq->fp);
            fileq->fp = NULL;
            return -1;
        }
    }

    /* File change time */
    if (fileq->fp) {
        if (fstat(fileno(fileq->fp), &fileq->f_status) < 0) {
            merror(FSTAT_ERROR, fileq->file_name, errno, strerror(errno));
            fclose(fileq->fp);
            fileq->fp = NULL;
            return -1;
        }
    }

    fileq->last_change = fileq->f_status.st_mtime;

    return 1;
}

// Close queue
void jqueue_close(file_queue * queue) {
    fclose(queue->fp);
    queue->fp = NULL;
}

/* Return alert data for the next file in the queue */
alert_data *GetAlertJSONData(file_queue *fileq){
    alert_data *al_data;
    cJSON *al_json;
    char *groups;
    int i = 0;

    cJSON *json_object;
    cJSON *rule;
    cJSON *syscheck;
    cJSON *location;
    cJSON *srcip;

    os_calloc(1, sizeof(alert_data), al_data);

    /* Get message if available */
    al_json = jqueue_next(fileq);
    
    if (!al_json) {
        minfo("Poh no hay na");
        return NULL;
    }

    if (!fileq->fp) {
        return NULL;
    }

    /* Rule */
    rule = cJSON_GetObjectItem(al_json, "rule");
    
    if (!rule) {
        cJSON_free(al_json);
        return NULL;
    }

    // Rule ID
    json_object = cJSON_GetObjectItem(rule, "id");

    if (json_object) {
        al_data->alertid = strdup(json_object->valuestring);
    }
    
    // Groups
    json_object = cJSON_GetObjectItem(rule, "groups");

    if (json_object) {
        /* Groups is an array in the alerts.json file */
        /*
            First, we copy the first item in groups, then the rest,
            in case there is more than one
        */
        os_calloc(1, strlen(cJSON_GetArrayItem(json_object, 0)->valuestring), groups);
        strcpy(groups, cJSON_GetArrayItem(json_object, 0)->valuestring);

        for (i = 1; i < cJSON_GetArraySize(json_object); i++){
            os_realloc(groups, strlen(groups) + strlen(cJSON_GetArrayItem(json_object, i)->valuestring) + 1, groups);    // +1 because of the comma
            strcat(groups, ",");
            strcat(groups, cJSON_GetArrayItem(json_object, i)->valuestring);
        }

        al_data->group = strdup(groups);

        os_free(groups);
    }

    // Level
    json_object = cJSON_GetObjectItem(rule, "level");

    if (json_object) {
        al_data->level = json_object->valueint;
    }

    /* Syscheck */
    syscheck = cJSON_GetObjectItem(al_json, "syscheck");

    if (syscheck) {
        // Path
        json_object = cJSON_GetObjectItem(syscheck, "path");

        if (json_object) {
            al_data->filename = strdup(json_object->valuestring);
        }
        
        // User
        json_object = cJSON_GetObjectItem(syscheck, "uname_after");

        if (json_object) {
            al_data->user = strdup(json_object->valuestring);
        }
    }

    /* Srcip */
    srcip = cJSON_GetObjectItem(al_json, "srcip");

    if (srcip) {
        al_data->srcip = strdup(json_object->valuestring);
    }

    /* Location */
    location = cJSON_GetObjectItem(al_json, "location");

    if (location) {
        al_data->location = strdup(location->valuestring);
    }

    return al_data;
}

/* Read from monitored file in JSON format */
alert_data *Read_JSON_Mon(file_queue *fileq, const struct tm *p, unsigned int timeout){
    unsigned int i = 0;
    alert_data *al_data;

    /* If the file queue is not available, try to access it */
    if (!fileq->fp) {
        if (Handle_JQueue(fileq, 0) != 1) {
            file_sleep();
            return NULL;
        }
    }

    if(!fileq->fp){
        return NULL;
    }

    al_data = GetAlertJSONData(fileq);

    if (al_data) {
        return al_data;
    }

    fileq->day = p->tm_mday;
    fileq->year = p->tm_year + 1900;
    strncpy(fileq->mon, s_month[p->tm_mon], 3);

    if (Handle_JQueue(fileq, 0) != 1) {
        file_sleep();
        return NULL;
    }

    /* Try up to timeout times to get an event */
    while (i < timeout) {
        al_data = GetAlertJSONData(fileq);

        if (al_data) {
            return al_data;
        }

        i++;
        file_sleep();
    }

    /* Return NULL if timeout expires */
    return NULL;
}
