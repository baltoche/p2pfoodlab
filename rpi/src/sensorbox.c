/* 

    P2P Food Lab Sensorbox

    Copyright (C) 2013  Sony Computer Science Laboratory Paris
    Author: Peter Hanappe

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <dirent.h> 
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "json.h"
#include "log_message.h"
#include "config.h"
#include "event.h"
#include "arduino.h"
#include "camera.h"
#include "network.h"
#include "opensensordata.h"
#include "sensorbox.h"

struct _sensorbox_t {
        char* home_dir;
        json_object_t config;
        arduino_t* arduino;
        camera_t* camera;
        opensensordata_t* osd;
        event_t* events;
        char filenamebuf[2048];
        int test;
        FILE* datafp;
        int lock;
};

static int sensorbox_load_config(sensorbox_t* box);
static char* sensorbox_path(sensorbox_t* box, const char* filename);
static int sensorbox_init_osd(sensorbox_t* box);
static int sensorbox_add_periodic_events(sensorbox_t* box, int period, int type);
static int sensorbox_add_fixed_events(sensorbox_t* box, json_object_t fixed, int type);
static int sensorbox_init_arduino(sensorbox_t* box);
static int sensorbox_init_camera(sensorbox_t* box);
static void sensorbox_handle_event(sensorbox_t* box, event_t* e, time_t t);
static void sensorbox_poweroff(sensorbox_t* box, int minutes);

sensorbox_t* new_sensorbox(const char* dir)
{
        sensorbox_t* box = (sensorbox_t*) malloc(sizeof(sensorbox_t));
        if (box == NULL) { 
                log_err("Sensorbox: out of memory");
                return NULL;
        }
        memset(box, 0, sizeof(sensorbox_t));

        box->home_dir = strdup(dir);

        if (sensorbox_load_config(box) != 0) {
                delete_sensorbox(box);
                return NULL;
        }

        if (sensorbox_init_arduino(box) != 0) {
                delete_sensorbox(box);
                return NULL;
        }

        if (sensorbox_init_camera(box) != 0) {
                delete_sensorbox(box);
                return NULL;
        }

        if (sensorbox_init_osd(box) != 0) {
                delete_sensorbox(box);
                return NULL;
        }

        box->lock = -1;

        return box;
}

int delete_sensorbox(sensorbox_t* box)
{
        json_unref(box->config);
        if (box->osd)
                delete_opensensordata(box->osd);
        if (box->camera)
                delete_camera(box->camera);
        eventlist_delete_all(box->events);
        free(box);
        return 0;
}

static char* sensorbox_path(sensorbox_t* box, const char* filename)
{
        int len = sizeof(box->filenamebuf);
        snprintf(box->filenamebuf, len, "%s/%s", box->home_dir, filename);
        box->filenamebuf[len-1] = 0;
        return box->filenamebuf;
}

static int sensorbox_load_config(sensorbox_t* box)
{
        box->config = config_load(sensorbox_path(box, "etc/config.json"));
        if (json_isnull(box->config)) {
                log_err("Sensorbox: Failed to load the config file"); 
                return -1;
        } 
        return 0;
}

static int sensorbox_init_osd(sensorbox_t* box)
{
        json_object_t osd_config = json_object_get(box->config, "opensensordata");
        if (!json_isobject(osd_config)) {
                log_err("Sensorbox: OpenSensorData settings are not a JSON object, as expected"); 
                return -1;
        }
        json_object_t server = json_object_get(osd_config, "server");
        if (!json_isstring(server)) {
                log_err("Sensorbox: OpenSensorData server setting is not a JSON string, as expected"); 
                return -1;
        }
        json_object_t key = json_object_get(osd_config, "key");
        if (!json_isstring(key)) {
                log_err("Sensorbox: OpenSensorData key is not a JSON string, as expected"); 
                return -1;
        }

        box->osd = new_opensensordata(json_string_value(server));
        if (box->osd == NULL) {
                log_err("Sensorbox: Out of memory"); 
                return -1;
        }

        opensensordata_set_key(box->osd, json_string_value(key));
        opensensordata_set_cache_dir(box->osd, sensorbox_path(box, "etc/opensensordata"));

        return 0;
}

static int get_image_size(const char* symbol, unsigned int* width, unsigned int* height)
{
        typedef struct _image_size_t {
                const char* symbol;
                unsigned int width;
                unsigned int height;
        } image_size_t;
        
        static image_size_t image_sizes[] = {
                { "320x240", 320, 240 },
                { "640x480", 640, 480 },
                { "960x720", 960, 720 },
                { "1024x768", 1024, 768 },
                { "1280x720", 1280, 720 },
                { "1280x960", 1280, 960 },
                { "1920x1080", 1920, 1080 },
                { NULL, 0, 0 }};

        for (int i = 0; image_sizes[i].symbol != 0; i++) {
                if (strcmp(image_sizes[i].symbol, symbol) == 0) {
                        *width = image_sizes[i].width;
                        *height = image_sizes[i].height;
                        return 0;
                }
        }

        return -1;
}

static int sensorbox_add_periodic_events(sensorbox_t* box, int period, int type)
{
        int minutes_day = 24 * 60;
        int minute = 0;
        while (minute < minutes_day) {
                event_t* e = new_event(minute, type);
                if (e == NULL)
                        return -1;
                box->events = eventlist_insert(box->events, e);
                minute += period;
        }
        return 0;
}

static int sensorbox_add_fixed_events(sensorbox_t* box, json_object_t fixed, int type)
{
        int num = json_array_length(fixed);
        for (int i = 0; i < num; i++) {
                json_object_t time = json_array_get(fixed, i);
                if (!json_isobject(time)) {
                        log_err("Sensorbox: Camera fixed update time setting is invalid"); 
                        continue;
                }
                json_object_t hs = json_object_get(time, "h");
                if (!json_isstring(hs)) {
                        log_err("Sensorbox: Camera fixed update time setting is invalid"); 
                        continue;
                }
                if (json_string_equals(hs, "")) {
                        continue;
                }
                int h = atoi(json_string_value(hs));
                if ((h < 0) || (h > 23)) {
                        log_err("Sensorbox: Invalid camera update hour: %d", h); 
                        continue;
                }
                json_object_t ms = json_object_get(time, "m");
                if (!json_isstring(ms)) {
                        log_err("Sensorbox: Camera fixed update time setting is invalid"); 
                        continue;
                }
                if (json_string_equals(ms, "")) {
                        continue;
                }
                int m = atoi(json_string_value(ms));
                if ((m < 0) || (m > 59)) {
                        log_err("Sensorbox: Invalid camera update minute: %d", m); 
                        continue;
                }
                event_t* e = new_event(h * 60 + m, type);
                if (e == NULL)
                        return -1;
                box->events = eventlist_insert(box->events, e);
        }
        return 0;
}

static int sensorbox_init_camera(sensorbox_t* box)
{
        json_object_t camera_obj = json_object_get(box->config, "camera");
        if (json_isnull(camera_obj)) {
                log_err("Sensorbox: Could not find the camera configuration"); 
                return -1;
        }
        json_object_t enabled = json_object_get(camera_obj, "enable");
        if (!json_isstring(enabled)) {
                log_err("Sensorbox: Camera enabled setting is not a JSON string, as expected"); 
                return -1;
        }
        if (!json_string_equals(enabled, "yes")) {
                log_debug("Sensorbox: Camera not enabled");
                return 0;
        }

        json_object_t update = json_object_get(camera_obj, "update");
        if (!json_isstring(update)) {
                log_err("Sensorbox: Camera update setting is not a JSON string, as expected"); 
                return -1;
        }

        if (json_string_equals(update, "fixed")) {
                json_object_t fixed = json_object_get(camera_obj, "fixed");
                if (!json_isarray(fixed)) {
                        log_err("Sensorbox: Camera fixed settings are not a JSON array, as expected"); 
                        return -1;
                }
                if (sensorbox_add_fixed_events(box, fixed, UPDATE_CAMERA) != 0)
                        return -1;

        } else if (json_string_equals(update, "periodical")) {
                json_object_t period = json_object_get(camera_obj, "period");
                if (!json_isstring(period)) {
                        log_err("Sensorbox: Camera period setting is not a JSON string, as expected"); 
                        return -1;
                }
                int value = atoi(json_string_value(period));
                if (value <= 0) {
                        log_err("Sensorbox: Invalid camera period setting: %d", value); 
                        return -1;
                }
                if (sensorbox_add_periodic_events(box, value, UPDATE_CAMERA) != 0)
                        return -1;
                        

        } else {
                log_err("Sensorbox: Invalid camera update setting: '%s'", 
                           json_string_value(update)); 
                return -1;
        }        

        json_object_t device_str = json_object_get(camera_obj, "device");
        if (!json_isstring(device_str)) {
                log_err("Sensorbox: Invalid device configuration"); 
                return -1;
        }        
        const char* device = json_string_value(device_str);

        json_object_t size_str = json_object_get(camera_obj, "size");
        if (!json_isstring(size_str)) {
                log_err("Sensorbox: Image size is not a string"); 
                return -1;
        }        

        unsigned int width, height;

        if (get_image_size(json_string_value(size_str), &width, &height) != 0) {
                log_err("Sensorbox: Invalid image size: %s", 
                        json_string_value(size_str)); 
                return -1;
        }

        box->camera = new_camera(device, IO_METHOD_MMAP,
                                 width, height, 90);

        if (box->camera == NULL)
                return -1;

        return 0;
}

static int sensorbox_init_arduino(sensorbox_t* box)
{
        static int bus = 1;
        static int address = 0x04;

        box->arduino = new_arduino(bus, address);
        if (box->arduino == NULL)
                return -1;

        json_object_t sensors = json_object_get(box->config, "sensors");
        if (!json_isobject(sensors)) {
                log_err("Sensorbox: Sensors settings are not a JSON object, as expected"); 
                return -1;
        }
        json_object_t upload = json_object_get(sensors, "upload");
        if (!json_isstring(upload)) {
                log_err("Sensorbox: Sensors upload setting is not a JSON string, as expected"); 
                return -1;
        }

        if (json_string_equals(upload, "fixed")) {
                json_object_t fixed = json_object_get(sensors, "fixed");
                if (!json_isarray(fixed)) {
                        log_err("Sensorbox: Sensors fixed settings are not a JSON array, as expected"); 
                        return -1;
                }
                if (sensorbox_add_fixed_events(box, fixed, UPDATE_SENSORS) != 0)
                        return -1;

        } else if (json_string_equals(upload, "periodical")) {
                json_object_t period = json_object_get(sensors, "period");
                if (!json_isstring(period)) {
                        log_err("Sensorbox: Sensors period setting is not a JSON string, as expected"); 
                        return -1;
                }
                int value = atoi(json_string_value(period));
                if (value <= 0) {
                        log_err("Sensorbox: Invalid sensors period setting: %d", value); 
                        return -1;
                }
                if (sensorbox_add_periodic_events(box, value, UPDATE_SENSORS) != 0)
                        return -1;

        } else {
                log_err("Sensorbox: Invalid sensors update setting: '%s'", 
                        json_string_value(upload)); 
                return -1;
        }        

        return 0;
}

static void sensorbox_handle_event(sensorbox_t* box, event_t* e, time_t t)
{
        if (e->type == UPDATE_SENSORS) {
                sensorbox_store_sensor_data(box, NULL);
        } else if (e->type == UPDATE_CAMERA) {
                sensorbox_update_camera(box, t);
        }
}

void sensorbox_grab_image(sensorbox_t* box, const char* filename)
{
        if (box->camera == NULL)
                return;

        int error = camera_capture(box->camera);
        if (error) {
                log_err("Sensorbox: Failed to grab the image"); 
                return;
        }

        log_info("Sensorbox: Storing photo in %s", filename);

        int size = camera_getimagesize(box->camera);
        unsigned char* buffer = camera_getimagebuffer(box->camera);

        FILE* fp = NULL;

        if (strcmp(filename, "-") == 0) {
                fp = stdout;
        } else {
                fp = fopen(filename, "w");
        }


        if (fp == NULL) {
                log_info("Sensorbox: Failed to open file '%s'", filename);
                return;
        }

        size_t n = 0;
        while (n < size) {
                size_t m = fwrite(buffer + n, 1, size - n, fp);
                if ((m == 0) && (ferror(fp) != 0)) { 
                        fclose(fp);
                        log_info("Sensorbox: Failed to write to file '%s'", 
                                 filename);
                        return;
                        
                }
                n += m;
        }

        fclose(fp);

        log_info("Sensorbox: Photo capture finished");
}

void sensorbox_update_camera(sensorbox_t* box, time_t t)
{
        char id[512];
        struct tm tm;

        localtime_r(&t, &tm);
        snprintf(id, 512, "%04d%02d%02d-%02d%02d%02d.jpg",
                 1900 + tm.tm_year, 1 + tm.tm_mon, tm.tm_mday, 
                 tm.tm_hour, tm.tm_min, tm.tm_sec);

        char filename[512];
        snprintf(filename, 512, "photostream/%s", id);

        char* path = sensorbox_path(box, filename);

        sensorbox_grab_image(box, path);
}

static int sensorbox_map_datastreams(sensorbox_t* box,
                                     unsigned char enabled, 
                                     int* ids)
{
        memset(ids, 0, DATASTREAM_LAST * sizeof(int));

        if (enabled & SENSOR_TRH) {
                ids[DATASTREAM_T] = opensensordata_get_datastream_id(box->osd, "t");
                ids[DATASTREAM_RH] = opensensordata_get_datastream_id(box->osd, "rh");
        }
        if (enabled & SENSOR_TRHX) {
                ids[DATASTREAM_TX] = opensensordata_get_datastream_id(box->osd, "tx");
                ids[DATASTREAM_RHX] = opensensordata_get_datastream_id(box->osd, "rhx");
        }
        if (enabled & SENSOR_LUM) {
                ids[DATASTREAM_LUM] = opensensordata_get_datastream_id(box->osd, "lum");
        }
        if (enabled & SENSOR_SOIL) {
                ids[DATASTREAM_SOIL] = opensensordata_get_datastream_id(box->osd, "soil");
        }

        for (int i = 0; i < DATASTREAM_LAST; i++) 
                if (ids[i] == -1)
                        return -1;

        return 0;
}

int sensorbox_check_sensors(sensorbox_t* box)
{
        unsigned char sensors_a;
        unsigned char period_a;
        unsigned char sensors_c;
        unsigned char period_c;
        int err;

        err = config_get_sensors(box->config, &sensors_c, &period_c);
        if (err != 0) 
                return err;
        
        err = arduino_get_sensors(box->arduino, &sensors_a);
        if (err != 0) 
                return err;
        
        err = arduino_get_period(box->arduino, &period_a);
        if (err != 0) 
                return err;

        log_info("Sensorbox: Arduino: sensors: 0x%02x, period %d", sensors_a, period_a); 
        log_info("Sensorbox: Config:  sensors: 0x%02x, period %d", sensors_c, period_c); 

        if (sensors_c != sensors_a) {
                log_info("Sensorbox: Sensor settings differ between Arduino and config file"); 
                err = arduino_set_sensors(box->arduino, sensors_c);
        }
        if (period_c != period_a) {
                log_info("Sensorbox: Period settings differ between Arduino and config file"); 
                err = arduino_set_period(box->arduino, period_c);
        }

        return err;
}

int sensorbox_store_sensor_data(sensorbox_t* box, 
                                const char* filename)
{
        unsigned char sensors_a;
        unsigned char period_a;
        int err;
        int osd_id[DATASTREAM_LAST];

        err = arduino_get_sensors(box->arduino, &sensors_a);
        if (err != 0) 
                return err;

        err = arduino_get_period(box->arduino, &period_a);
        if (err != 0) 
                return err;

        err = sensorbox_map_datastreams(box, sensors_a, osd_id);
        if (err != 0) 
                return err;

        if (filename == NULL) {
                filename = sensorbox_path(box, "datapoints.csv");
                box->datafp = fopen(filename, "a");

        } else if (strcmp(filename, "-") == 0) {
                box->datafp = stdout;

        } else {
                box->datafp = fopen(filename, "a");
        }

        if (box->datafp == NULL) {
                log_err("Arduino: Failed to open the output file: %s", 
                        filename);
                return -1;
        }

        int num_points;
        datapoint_t* datapoints = arduino_read_data(box->arduino, &num_points);

        for (int i = 0; i < num_points; i++) {
                struct tm r;
                char s[256];
                
                localtime_r(&datapoints[i].timestamp, &r);
                snprintf(s, 256, "%04d-%02d-%02dT%02d:%02d:%02d",
                         1900 + r.tm_year, 1 + r.tm_mon, r.tm_mday, 
                         r.tm_hour, r.tm_min, r.tm_sec);
                
                fprintf(box->datafp, "%d,%s,%f\n", 
                        osd_id[datapoints[i].datastream], 
                        s, 
                        datapoints[i].value);
        } 

        if (datapoints)
                free(datapoints);

        if (box->datafp != stdout) {
                fclose(box->datafp);
                box->datafp = NULL;
        }

        return err;
}

void sensorbox_test_run(sensorbox_t* box)
{
        box->test = 1;
}

void sensorbox_handle_events(sensorbox_t* box)
{
        int err;
        time_t t;
        struct tm tm;

        err = arduino_get_time(box->arduino, &t);
        if (err != 0)
                return;
        //t = time(NULL);
        localtime_r(&t, &tm);
        log_debug("Current time: %02d:%02d.", tm.tm_hour, tm.tm_min);
        int cur_minute = tm.tm_hour * 60 + tm.tm_min;

        event_t* e = eventlist_get_next(box->events, cur_minute);
        if (e == NULL) {
                log_info("No more events for today."); 
                return;
        }

        while ((e != NULL) &&
               (e->minute == cur_minute)) {
                if (box->test) 
                        log_info("EXEC update %s", (e->type == UPDATE_SENSORS)? "sensors" : "camera");
                else
                        sensorbox_handle_event(box, e, t);
                e = e->next;
        }
}

void sensorbox_upload_data(sensorbox_t* box)
{
        struct stat buf;

        char* filename = sensorbox_path(box, "datapoints.csv");

        if (stat(filename, &buf) == -1) {
                log_debug("Sensorbox: No datapoints to upload");
                return;
        }
        if ((buf.st_mode & S_IFMT) != S_IFREG) {
                log_info("Sensorbox: Datapoints file is not a regular file!");
                return;
        }
        if (buf.st_size == 0) {
                log_debug("Sensorbox: Datapoints file is empty");
                return;
        }
        if (box->test) {
                log_debug("Sensorbox: Upload datapoints (filesize=%d)", (int) buf.st_size); 
                return;
        }

        log_info("Sensorbox: Uploading datapoints (filesize=%d)", (int) buf.st_size); 

        if (sensorbox_bring_network_up(box) != 0) {
                log_err("Sensorbox: Failed to bring the network up");
                return;
        }

        int ret = opensensordata_put_datapoints(box->osd, filename);
        if (ret != 0) {
                log_err("Sensorbox: Uploading of datapoints failed"); 
                char* resp = opensensordata_get_response(box->osd);
                if (resp) 
                        log_err("%s", resp); 
                return;
        } 

        log_info("Sensorbox: Upload successful"); 

        char backupfile[512];
        struct timeval tv;
        struct tm r;

        gettimeofday(&tv, NULL);
        localtime_r(&tv.tv_sec, &r);
        snprintf(backupfile, 512, "%s/backup/datapoints-%04d%02d%02d-%02d%02d%02d.csv",
                 box->home_dir,
                 1900 + r.tm_year, 1 + r.tm_mon, r.tm_mday, 
                 r.tm_hour, r.tm_min, r.tm_sec);
        backupfile[511] = 0;

        log_info("Sensorbox: Copying datapoints to %s", backupfile);
        
        if (rename(filename, backupfile) == -1) {
                log_err("Sensorbox: Failed to copy datapoints to %s", backupfile); 
        }        
}

void sensorbox_upload_photos(sensorbox_t* box)
{
        DIR *dir;
        struct dirent *entry;
        struct stat buf;
        char filename[512];
        char backupfile[512];

        char* dirname = sensorbox_path(box, "photostream");
        
        dir = opendir(dirname);
        if (dir == NULL) {
                log_err("Sensorbox: Failed to open the photo directory '%s'", dirname);
                return;
        }

        int count = 0;
        while ((entry = readdir(dir)) != NULL) {
                snprintf(filename, 511, "%s/%s", dirname, entry->d_name);
                filename[511] = 0;
        
                if ((stat(filename, &buf) == -1)
                    || ((buf.st_mode & S_IFMT) != S_IFREG)
                    || (buf.st_size == 0))
                        continue;

                count++;
        }

        if (count == 0) {
                closedir(dir);
                return;
        }

        log_info("Sensorbox: Found %d %s on the disk", 
                 count, (count == 1)? "photo" : "photos");

        int photostream = opensensordata_get_datastream_id(box->osd, "webcam");
        
        if (sensorbox_bring_network_up(box) != 0) {
                log_err("Sensorbox: Failed to bring the network up");
                return;
        }

        rewinddir(dir);

        while ((entry = readdir(dir)) != NULL) {
                snprintf(filename, 511, "%s/%s", dirname, entry->d_name);
                filename[511] = 0;
        
                if ((stat(filename, &buf) == -1)
                    || ((buf.st_mode & S_IFMT) != S_IFREG)
                    || (buf.st_size == 0))
                        continue;

                log_info("Sensorbox: Uploading photo '%s'", filename);

                if (box->test)
                        continue;

                int err = opensensordata_put_photo(box->osd, photostream, 
                                                   entry->d_name, filename);
                if (err != 0) {
                        log_err("Sensorbox: Uploading of photo failed"); 
                        char* resp = opensensordata_get_response(box->osd);
                        if (resp) 
                                log_err("%s", resp); 
                        continue;
                }

                snprintf(backupfile, 512, "%s/backup/%s", box->home_dir, entry->d_name);
                backupfile[511] = 0;

                log_info("Sensorbox: Copying photo to %s", backupfile);
                
                if (rename(filename, backupfile) == -1) {
                        log_err("Sensorbox: Failed to copy photo to %s", backupfile); 
                }
        }
        
        closedir(dir);
}

int sensorbox_powersaving_enabled(sensorbox_t* box)
{
        return config_powersaving_enabled(box->config);
}

static void sensorbox_poweroff(sensorbox_t* box, int minutes)
{
        int err = arduino_set_poweroff(box->arduino, minutes);
        if (err != 0) 
                return;

        if (1) {
                log_info("Sensorbox: Powering off"); 
                char* argv[] = { "/usr/bin/sudo", "/sbin/poweroff", NULL};
                execv(argv[0], argv);
                log_err("Sensorbox: Failed to poweroff");         

        } else {
                log_info("Sensorbox: Powering off, NOT"); 
        }
}

void sensorbox_poweroff_maybe(sensorbox_t* box)
{
        int err;
        time_t t;
        struct tm tm;

        //t = time(NULL);
        err = arduino_get_time(box->arduino, &t);
        if (err != 0)
                return;

        localtime_r(&t, &tm);
        log_debug("Current time: %02d:%02d.", tm.tm_hour, tm.tm_min);
        int cur_minute = tm.tm_hour * 60 + tm.tm_min;

        event_t* e = eventlist_get_next(box->events, cur_minute + 1);
        if (e == NULL)
                e = eventlist_get_next(box->events, 0);
        if (e == NULL)
                return;

        int delta;
        if (e->minute < cur_minute) 
                delta = e->minute + 24 * 60 - cur_minute;
        else
                delta = e->minute - cur_minute;

        if (delta == 0) {
                return;
        } else if (delta == 1) {
                log_info("Next event in 1 minute");
                return;
        } else if (delta <= 5) {
                log_info("Next event in %d minute(s)", delta);
        } else if ((delta < 60) && ((delta % 10) == 0)) {
                log_info("Next event in %d minute(s)", delta);
        } else if ((delta >= 60) && (delta % 60) == 0) {
                log_info("Next event in %d minute(s)", delta);
        }  

        /* Power off if:
           1. poweroff enabled in config
           2. next event is at least 3 minutes in the future
           3. the system has been up for at least 3 minutes.

           The last criteria is to avoid that the sensorbox shuts down
           too quickly to allow people to connect to it.
         */
        int uptime = sensorbox_uptime(box);
        int enabled = sensorbox_powersaving_enabled(box);
        if (!enabled) 
                return;

        if ((delta > 3) && (uptime > 180)) {
                if (box->test) 
                        printf("POWEROFF %d\n", delta - 3);
                else
                        sensorbox_poweroff(box, delta - 3);
        } else if (delta <= 3) {
                log_info("Not powering off, next event is coming soon");
        } else if (uptime <= 180) {
                log_info("Not powering off, system just started (%ds < 180s)", uptime);
        }
}

int sensorbox_get_time(sensorbox_t* box, time_t* m) 
{
        return arduino_get_time(box->arduino, m);
}

int sensorbox_set_time(sensorbox_t* box, time_t m) 
{
        return arduino_set_time(box->arduino, m);
}

const char* sensorbox_getdir(sensorbox_t* box)
{
        return box->home_dir;
}


int sensorbox_print_events(sensorbox_t* box)
{
        eventlist_print(box->events, stdout); 
        return 0;
}

int sensorbox_uptime(sensorbox_t* box)
{
        FILE* fp = fopen("/proc/uptime", "r");
        if (fp == NULL) {
                log_err("Failed to open /proc/uptime!");
                return -1;
        }
        double seconds;
        int n = fscanf(fp, "%lf", &seconds);
        if (n != 1) {
                log_err("Failed to read the system uptime!");
                fclose(fp);
                return -1;
        }
        fclose(fp);
        return (int) seconds;
}

int sensorbox_bring_network_up(sensorbox_t* box)
{
        const char* iface = config_get_network_interface(box->config);
        return network_gogo(iface);
}

int sensorbox_bring_network_down(sensorbox_t* box)
{
        const char* iface = config_get_network_interface(box->config);
        return network_byebye(iface);
}

void sensorbox_bring_network_down_maybe(sensorbox_t* box)
{
        const char* iface = config_get_network_interface(box->config);

        if (strcmp(iface, "eth0") == 0) {
                return;        
        } else if ((strcmp(iface, "wlan0") == 0)
            && sensorbox_powersaving_enabled(box)) {
                sensorbox_bring_network_down(box);
        } else if (strcmp(iface, "ppp0") == 0) {
                sensorbox_bring_network_down(box);
        }
}

int sensorbox_lock(sensorbox_t* box) 
{
        int fd;
        if ((fd = open("/tmp/sensorbox.lock", O_CREAT | O_RDWR, 0666))  < 0)
                return -1;
        
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
                close(fd);
                return -1;
        }
        box->lock = fd;
        return 0;
}

void sensorbox_unlock(sensorbox_t* box) 
{
        if (box->lock == -1)
                return;
        flock(box->lock, LOCK_UN);
        close(box->lock);
        box->lock = -1;
}
