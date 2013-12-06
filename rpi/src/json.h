/*
    json.c A small JSON parser.

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
#ifndef _JSON_H
#define _JSON_H

#if !defined(JSON_EMBEDDED)
#include <stdio.h>
#include <math.h>
#endif

typedef char int8;
typedef unsigned char uint8;
typedef short int16;
typedef long int32;
typedef unsigned long uint32;
typedef float float32;

#if defined(JSON_EMBEDDED)
typedef float float64;
#else
typedef double float64;
#endif


#ifdef __cplusplus
extern "C" {
#endif

// 

enum {
	k_json_null = 0,
	k_json_object = 1,
	k_json_array = 2,
	k_json_string = 3,
	k_json_number = 4,
	k_json_boolean = 5,
};

typedef struct _json_object_t {
	uint8 type;
	union {
		void* data;
		float64 number;
	} value;	
} json_object_t;


#define json_ref(_obj)   json_refcount(&(_obj), 1)
#define json_unref(_obj) json_refcount(&(_obj), -1)
void json_refcount(json_object_t *object, int32 val);

#if JSON_EMBEDDED
void json_init_memory(char* ptr, int32 size);
#endif

/* Return zero to continue, non-zero to stop the iteration. */
typedef int32 (*json_iterator_t)(const char* key, json_object_t* value, void* data);

typedef int32 (*json_writer_t)(void* userdata, const char* s, int32 len);

//

#define json_type(__o) __o.type

//

json_object_t json_load(const char* filename, char* err, int len);

// object

json_object_t json_object_create();
int32 json_object_length(json_object_t object);
/* Returns the code of the iterator function. */
int32 json_object_foreach(json_object_t object, json_iterator_t func, void* data);
json_object_t json_object_get(json_object_t object, const char* key);
int32 json_object_set(json_object_t object, const char* key, json_object_t value);
int32 json_object_unset(json_object_t object, const char* key);

double json_object_getnum(json_object_t object, char* key);
const char* json_object_getstr(json_object_t object, const char* key);

int32 json_object_setnum(json_object_t object, const char* key, double value);
int32 json_object_setstr(json_object_t object, const char* key, const char* value);

#define json_isobject(__obj) (__obj.type == k_json_object)

// serialisation


enum {
	k_json_pretty = 1,
	k_json_binary = 2,
};

int32 json_serialise(json_object_t object, 
                     int32 flags, 
                     json_writer_t fun, 
                     void* userdata);
int32 json_tostring(json_object_t object, char* buffer, int32 buflen);

#if !defined(JSON_EMBEDDED)
int32 json_tofile(json_object_t object, int32 flags, const char* path);
int32 json_tofilep(json_object_t object, int32 flags, FILE* fp);
#endif

// parsing
typedef struct _json_parser_t json_parser_t;

json_parser_t* json_parser_create();
void json_parser_destroy(json_parser_t* parser);
void json_parser_init(json_parser_t* parser);
void json_parser_cleanup(json_parser_t* parser);

void json_parser_reset(json_parser_t* parser);

int32 json_parser_errno(json_parser_t* parser);
char* json_parser_errstr(json_parser_t* parser);

int32 json_parser_feed(json_parser_t* parser, const char* buffer, int32 len);
int32 json_parser_feed_one(json_parser_t* parser, char c);
int32 json_parser_done(json_parser_t* parser);

json_object_t json_parser_eval(json_parser_t* parser, const char* buffer);

json_object_t json_parser_result(json_parser_t* parser);



// null

json_object_t json_null();

#define json_isnull(__obj) (__obj.type == k_json_null)

// boolean

json_object_t json_true();
json_object_t json_false();

// number

json_object_t json_number_create(double value);
float64 json_number_value(json_object_t);

#define json_isnumber(__obj) (__obj.type == k_json_number)

// string

json_object_t json_string_create(const char* s);
const char* json_string_value(json_object_t string);
int32 json_string_length(json_object_t string);
int32 json_string_equals(json_object_t string, const char* s);

#define json_isstring(__obj) (__obj.type == k_json_string)

// array

json_object_t json_array_create();
int32 json_array_length(json_object_t array);
json_object_t json_array_get(json_object_t array, int32 index);
int32 json_array_set(json_object_t array, json_object_t value, int32 index);
int32 json_array_push(json_object_t array, json_object_t value);

int32 json_array_gettype(json_object_t object, int32 index);

float64 json_array_getnum(json_object_t object, int32 index);
const char* json_array_getstr(json_object_t object, int32 index);

int32 json_array_setnum(json_object_t object, double value, int32 index);
int32 json_array_setstr(json_object_t object, char* value, int32 index);

#define json_isarray(__obj) (__obj.type == k_json_array)

#ifdef __cplusplus
}
#endif

#endif // _JSON_H