/*
  Copyright (c) 2013, Dan VerWeire<dverweire@gmail.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <string.h>
#include <v8.h>
#include <node.h>
#include <node_version.h>
#include <node_buffer.h>
#include <time.h>
#include <uv.h>

#include "odbc.h"
#include "odbc_connection.h"
#include "odbc_result.h"
#include "odbc_statement.h"

using namespace v8;
using namespace node;

Persistent<FunctionTemplate> ODBCResult::constructor_template;
Persistent<String> ODBCResult::OPTION_FETCH_MODE = Persistent<String>::New(String::New("fetchMode"));

void ODBCResult::Init(v8::Handle<Object> target) {
  DEBUG_PRINTF("ODBCResult::Init\n");
  HandleScope scope;

  Local<FunctionTemplate> t = FunctionTemplate::New(New);

  // Constructor Template
  constructor_template = Persistent<FunctionTemplate>::New(t);
  constructor_template->SetClassName(String::NewSymbol("ODBCResult"));

  // Reserve space for one Handle<Value>
  Local<ObjectTemplate> instance_template = constructor_template->InstanceTemplate();
  instance_template->SetInternalFieldCount(1);
  
  // Prototype Methods
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "fetchAll", FetchAll);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "fetch", Fetch);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "getColumnValue", GetColumnValue);

  NODE_SET_PROTOTYPE_METHOD(constructor_template, "moreResultsSync", MoreResultsSync);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "closeSync", CloseSync);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "fetchSync", FetchSync);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "fetchAllSync", FetchAllSync);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "getColumnNamesSync", GetColumnNamesSync);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "getColumnValueSync", GetColumnValueSync);

  // Properties
  instance_template->SetAccessor(String::New("fetchMode"), FetchModeGetter, FetchModeSetter);
  
  // Attach the Database Constructor to the target object
  target->Set( v8::String::NewSymbol("ODBCResult"),
               constructor_template->GetFunction());
  
  scope.Close(Undefined());
}

ODBCResult::~ODBCResult() {
  this->Free();
}

void ODBCResult::Free() {
  DEBUG_PRINTF("ODBCResult::Free m_hSTMT=%X m_canFreeHandle=%X\n", m_hSTMT, m_canFreeHandle);
  
  if (m_hSTMT && m_canFreeHandle) {
    uv_mutex_lock(&ODBC::g_odbcMutex);
    
    SQLFreeHandle( SQL_HANDLE_STMT, m_hSTMT);
    
    m_hSTMT = NULL;
  
    uv_mutex_unlock(&ODBC::g_odbcMutex);
  }
  
  if (bufferLength > 0) {
    bufferLength = 0;
    free(buffer);
  }
}

Handle<Value> ODBCResult::New(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::New\n");
  
  HandleScope scope;
  
  REQ_EXT_ARG(0, js_henv);
  REQ_EXT_ARG(1, js_hdbc);
  REQ_EXT_ARG(2, js_hstmt);
  REQ_EXT_ARG(3, js_canFreeHandle);
  
  HENV hENV = static_cast<HENV>(js_henv->Value());
  HDBC hDBC = static_cast<HDBC>(js_hdbc->Value());
  HSTMT hSTMT = static_cast<HSTMT>(js_hstmt->Value());
  bool* canFreeHandle = static_cast<bool *>(js_canFreeHandle->Value());
  
  //create a new OBCResult object
  ODBCResult* objODBCResult = new ODBCResult(hENV, hDBC, hSTMT, *canFreeHandle);
  
  DEBUG_PRINTF("ODBCResult::New m_hDBC=%X m_hDBC=%X m_hSTMT=%X canFreeHandle=%X\n",
    objODBCResult->m_hENV,
    objODBCResult->m_hDBC,
    objODBCResult->m_hSTMT,
    objODBCResult->m_canFreeHandle
  );
  
  //free the pointer to canFreeHandle
  delete canFreeHandle;

  //specify the buffer length
  objODBCResult->bufferLength = MAX_VALUE_SIZE - 1;
  
  //initialze a buffer for this object
  objODBCResult->buffer = (uint16_t *) malloc(objODBCResult->bufferLength + 1);
  //TODO: make sure the malloc succeeded

  //set the initial colCount to 0
  objODBCResult->colCount = 0;

  //default fetchMode to FETCH_OBJECT
  objODBCResult->m_fetchMode = FETCH_OBJECT;
  
  objODBCResult->Wrap(args.Holder());
  
  return scope.Close(args.Holder());
}

Handle<Value> ODBCResult::FetchModeGetter(Local<String> property, const AccessorInfo &info) {
  HandleScope scope;

  ODBCResult *obj = ObjectWrap::Unwrap<ODBCResult>(info.Holder());

  return scope.Close(Integer::New(obj->m_fetchMode));
}

void ODBCResult::FetchModeSetter(Local<String> property, Local<Value> value, const AccessorInfo &info) {
  HandleScope scope;

  ODBCResult *obj = ObjectWrap::Unwrap<ODBCResult>(info.Holder());
  
  if (value->IsNumber()) {
    obj->m_fetchMode = value->Int32Value();
  }
}

/*
 * Fetch
 */

Handle<Value> ODBCResult::Fetch(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::Fetch\n");
  
  HandleScope scope;
  
  ODBCResult* objODBCResult = ObjectWrap::Unwrap<ODBCResult>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  fetch_work_data* data = (fetch_work_data *) calloc(1, sizeof(fetch_work_data));
  
  Local<Function> cb;
   
  //set the fetch mode to the default of this instance
  data->fetchMode = objODBCResult->m_fetchMode;
  
  if (args.Length() == 1 && args[0]->IsFunction()) {
    cb = Local<Function>::Cast(args[0]);
  }
  else if (args.Length() == 2 && args[0]->IsObject() && args[1]->IsFunction()) {
    cb = Local<Function>::Cast(args[1]);  
    
    Local<Object> obj = args[0]->ToObject();
    
    if (obj->Has(OPTION_FETCH_MODE) && obj->Get(OPTION_FETCH_MODE)->IsInt32()) {
      data->fetchMode = obj->Get(OPTION_FETCH_MODE)->ToInt32()->Value();
    }
  }
  else {
    return ThrowException(Exception::TypeError(
      String::New("ODBCResult::Fetch(): 1 or 2 arguments are required. The last argument must be a callback function.")
    ));
  }
  
  data->cb = Persistent<Function>::New(cb);
  
  data->objResult = objODBCResult;
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(), 
    work_req, 
    UV_Fetch, 
    (uv_after_work_cb)UV_AfterFetch);

  objODBCResult->Ref();

  return scope.Close(Undefined());
}

void ODBCResult::UV_Fetch(uv_work_t* work_req) {
  DEBUG_PRINTF("ODBCResult::UV_Fetch\n");
  
  fetch_work_data* data = (fetch_work_data *)(work_req->data);
  
  data->result = SQLFetch(data->objResult->m_hSTMT);
}

void ODBCResult::UV_AfterFetch(uv_work_t* work_req, int status) {
  DEBUG_PRINTF("ODBCResult::UV_AfterFetch\n");
  
  HandleScope scope;
  
  fetch_work_data* data = (fetch_work_data *)(work_req->data);
  
  SQLRETURN ret = data->result;
  //TODO: we should probably define this on the work data so we
  //don't have to keep creating it?
  Local<Object> objError;
  bool moreWork = true;
  bool error = false;
  
  if (data->objResult->colCount == 0) {
    data->objResult->columns = ODBC::GetColumns(
      data->objResult->m_hSTMT, 
      &data->objResult->colCount);
  }
  
  //check to see if the result has no columns
  if (data->objResult->colCount == 0) {
    //this means
    moreWork = false;
  }
  //check to see if there was an error
  else if (ret == SQL_ERROR)  {
    moreWork = false;
    error = true;
    
    objError = ODBC::GetSQLError(
      SQL_HANDLE_STMT, 
      data->objResult->m_hSTMT,
      (char *) "Error in ODBCResult::UV_AfterFetch");
  }
  //check to see if we are at the end of the recordset
  else if (ret == SQL_NO_DATA) {
    moreWork = false;
  }

  if (moreWork) {
    Handle<Value> args[3];

    args[0] = Null();
    if (data->fetchMode == FETCH_ARRAY) {
      args[1] = ODBC::GetRecordArray(
        data->objResult->m_hSTMT,
        data->objResult->columns,
        &data->objResult->colCount,
        data->objResult->buffer,
        data->objResult->bufferLength);
    }
    else if (data->fetchMode == FETCH_OBJECT) {
      args[1] = ODBC::GetRecordTuple(
        data->objResult->m_hSTMT,
        data->objResult->columns,
        &data->objResult->colCount,
        data->objResult->buffer,
        data->objResult->bufferLength);
    }
    else {
      args[1] = Null();
    }

    args[2] = True();

    TryCatch try_catch;

    data->cb->Call(Context::GetCurrent()->Global(), 3, args);
    data->cb.Dispose();

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }
  else {
    ODBC::FreeColumns(data->objResult->columns, &data->objResult->colCount);
    
    Handle<Value> args[3];
    
    //if there was an error, pass that as arg[0] otherwise Null
    if (error) {
      args[0] = objError;
    }
    else {
      args[0] = Null();
    }
    
    args[1] = Null();
    args[2] = False();

    TryCatch try_catch;

    data->cb->Call(Context::GetCurrent()->Global(), 3, args);
    data->cb.Dispose();

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }
  }
  
  free(data);
  free(work_req);
  
  data->objResult->Unref();
  
  return;
}

/*
 * FetchSync
 */

Handle<Value> ODBCResult::FetchSync(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::FetchSync\n");
  
  HandleScope scope;
  
  ODBCResult* objResult = ObjectWrap::Unwrap<ODBCResult>(args.Holder());

  Local<Object> objError;
  bool moreWork = true;
  bool error = false;
  int fetchMode = objResult->m_fetchMode;
  
  if (args.Length() == 1 && args[0]->IsObject()) {
    Local<Object> obj = args[0]->ToObject();
    
    if (obj->Has(OPTION_FETCH_MODE) && obj->Get(OPTION_FETCH_MODE)->IsInt32()) {
      fetchMode = obj->Get(OPTION_FETCH_MODE)->ToInt32()->Value();
    }
  }
  
  SQLRETURN ret = SQLFetch(objResult->m_hSTMT);

  if (objResult->colCount == 0) {
    objResult->columns = ODBC::GetColumns(
      objResult->m_hSTMT, 
      &objResult->colCount);
  }
  
  //check to see if the result has no columns
  if (objResult->colCount == 0) {
    moreWork = false;
  }
  //check to see if there was an error
  else if (ret == SQL_ERROR)  {
    moreWork = false;
    error = true;
    
    objError = ODBC::GetSQLError(
      SQL_HANDLE_STMT, 
      objResult->m_hSTMT,
      (char *) "Error in ODBCResult::UV_AfterFetch");
  }
  //check to see if we are at the end of the recordset
  else if (ret == SQL_NO_DATA) {
    moreWork = false;
  }

  if (moreWork) {
    Handle<Value> data;
    
    if (fetchMode == FETCH_ARRAY) {
      data = ODBC::GetRecordArray(
        objResult->m_hSTMT,
        objResult->columns,
        &objResult->colCount,
        objResult->buffer,
        objResult->bufferLength);
    }
    else if (fetchMode == FETCH_OBJECT) {
      data = ODBC::GetRecordTuple(
        objResult->m_hSTMT,
        objResult->columns,
        &objResult->colCount,
        objResult->buffer,
        objResult->bufferLength);
    } else {
      data = Null();
    }
    
    return scope.Close(data);
  }
  else {
    ODBC::FreeColumns(objResult->columns, &objResult->colCount);

    //if there was an error, pass that as arg[0] otherwise Null
    if (error) {
      ThrowException(objError);
      
      return scope.Close(Null());
    }
    else {
      return scope.Close(Null());
    }
  }
}

/*
 * FetchAll
 */

Handle<Value> ODBCResult::FetchAll(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::FetchAll\n");
  
  HandleScope scope;
  
  ODBCResult* objODBCResult = ObjectWrap::Unwrap<ODBCResult>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  fetch_work_data* data = (fetch_work_data *) calloc(1, sizeof(fetch_work_data));
  
  Local<Function> cb;
  
  data->fetchMode = objODBCResult->m_fetchMode;
  
  if (args.Length() == 1 && args[0]->IsFunction()) {
    cb = Local<Function>::Cast(args[0]);
  }
  else if (args.Length() == 2 && args[0]->IsObject() && args[1]->IsFunction()) {
    cb = Local<Function>::Cast(args[1]);  
    
    Local<Object> obj = args[0]->ToObject();
    
    if (obj->Has(OPTION_FETCH_MODE) && obj->Get(OPTION_FETCH_MODE)->IsInt32()) {
      data->fetchMode = obj->Get(OPTION_FETCH_MODE)->ToInt32()->Value();
    }
  }
  else {
    return ThrowException(Exception::TypeError(
      String::New("ODBCResult::FetchAll(): 1 or 2 arguments are required. The last argument must be a callback function.")
    ));
  }
  
  data->rows = Persistent<Array>::New(Array::New());
  data->errorCount = 0;
  data->count = 0;
  data->objError = Persistent<Object>::New(Object::New());
  
  data->cb = Persistent<Function>::New(cb);
  data->objResult = objODBCResult;
  
  work_req->data = data;
  
  uv_queue_work(uv_default_loop(),
    work_req, 
    UV_FetchAll, 
    (uv_after_work_cb)UV_AfterFetchAll);

  data->objResult->Ref();

  return scope.Close(Undefined());
}

void ODBCResult::UV_FetchAll(uv_work_t* work_req) {
  DEBUG_PRINTF("ODBCResult::UV_FetchAll\n");
  
  fetch_work_data* data = (fetch_work_data *)(work_req->data);
  
  data->result = SQLFetch(data->objResult->m_hSTMT);
 }

void ODBCResult::UV_AfterFetchAll(uv_work_t* work_req, int status) {
  DEBUG_PRINTF("ODBCResult::UV_AfterFetchAll\n");
  
  HandleScope scope;
  
  fetch_work_data* data = (fetch_work_data *)(work_req->data);
  
  ODBCResult* self = data->objResult->self();
  
  bool doMoreWork = true;
  
  if (self->colCount == 0) {
    self->columns = ODBC::GetColumns(self->m_hSTMT, &self->colCount);
  }
  
  //check to see if the result set has columns
  if (self->colCount == 0) {
    //this most likely means that the query was something like
    //'insert into ....'
    doMoreWork = false;
  }
  //check to see if there was an error
  else if (data->result == SQL_ERROR)  {
    data->errorCount++;
    
    data->objError = Persistent<Object>::New(ODBC::GetSQLError(
      SQL_HANDLE_STMT, 
      self->m_hSTMT,
      (char *) "[node-odbc] Error in ODBCResult::UV_AfterFetchAll"
    ));
    
    doMoreWork = false;
  }
  //check to see if we are at the end of the recordset
  else if (data->result == SQL_NO_DATA) {
    doMoreWork = false;
  }
  else {
    if (data->fetchMode == FETCH_ARRAY) {
      data->rows->Set(
        Integer::New(data->count), 
        ODBC::GetRecordArray(
          self->m_hSTMT,
          self->columns,
          &self->colCount,
          self->buffer,
          self->bufferLength)
      );
    }
    else if (data->fetchMode == FETCH_OBJECT) {
      data->rows->Set(
        Integer::New(data->count), 
        ODBC::GetRecordTuple(
          self->m_hSTMT,
          self->columns,
          &self->colCount,
          self->buffer,
          self->bufferLength)
      );
    }
    data->count++;
  }
  
  if (doMoreWork) {
    //Go back to the thread pool and fetch more data!
    uv_queue_work(
      uv_default_loop(),
      work_req, 
      UV_FetchAll, 
      (uv_after_work_cb)UV_AfterFetchAll);
  }
  else {
    ODBC::FreeColumns(self->columns, &self->colCount);
    
    Handle<Value> args[2];
    
    if (data->errorCount > 0) {
      args[0] = Local<Object>::New(data->objError);
    }
    else {
      args[0] = Null();
    }
    
    args[1] = Local<Array>::New(data->rows);

    TryCatch try_catch;

    data->cb->Call(Context::GetCurrent()->Global(), 2, args);
    data->cb.Dispose();
    data->rows.Dispose();
    data->objError.Dispose();

    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }

    //TODO: Do we need to free self->rows somehow?
    free(data);
    free(work_req);

    self->Unref(); 
  }
  
  scope.Close(Undefined());
}

/*
 * FetchAllSync
 */

Handle<Value> ODBCResult::FetchAllSync(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::FetchAllSync\n");

  HandleScope scope;
  
  ODBCResult* self = ObjectWrap::Unwrap<ODBCResult>(args.Holder());
  
  Local<Object> objError = Object::New();
  
  SQLRETURN ret;
  int count = 0;
  int errorCount = 0;
  int fetchMode = self->m_fetchMode;

  if (args.Length() == 1 && args[0]->IsObject()) {
    Local<Object> obj = args[0]->ToObject();
    
    if (obj->Has(OPTION_FETCH_MODE) && obj->Get(OPTION_FETCH_MODE)->IsInt32()) {
      fetchMode = obj->Get(OPTION_FETCH_MODE)->ToInt32()->Value();
    }
  }
  
  if (self->colCount == 0) {
    self->columns = ODBC::GetColumns(self->m_hSTMT, &self->colCount);
  }
  
  Local<Array> rows = Array::New();
  
  //Only loop through the recordset if there are columns
  if (self->colCount > 0) {
    //loop through all records
    while (true) {
      ret = SQLFetch(self->m_hSTMT);
      
      //check to see if there was an error
      if (ret == SQL_ERROR)  {
        errorCount++;
        
        objError = ODBC::GetSQLError(
          SQL_HANDLE_STMT, 
          self->m_hSTMT,
          (char *) "[node-odbc] Error in ODBCResult::UV_AfterFetchAll; probably"
            " your query did not have a result set."
        );
        
        break;
      }
      
      //check to see if we are at the end of the recordset
      if (ret == SQL_NO_DATA) {
        ODBC::FreeColumns(self->columns, &self->colCount);
        
        break;
      }

      if (fetchMode == FETCH_ARRAY) {
        rows->Set(
          Integer::New(count), 
          ODBC::GetRecordArray(
            self->m_hSTMT,
            self->columns,
            &self->colCount,
            self->buffer,
            self->bufferLength)
        );
      }
      else if (fetchMode == FETCH_OBJECT) {
        rows->Set(
          Integer::New(count), 
          ODBC::GetRecordTuple(
            self->m_hSTMT,
            self->columns,
            &self->colCount,
            self->buffer,
            self->bufferLength)
        );
      }
      count++;
    }
  }
  else {
    ODBC::FreeColumns(self->columns, &self->colCount);
  }
  
  //throw the error object if there were errors
  if (errorCount > 0) {
    ThrowException(objError);
  }
  
  return scope.Close(rows);
}

/*
 * CloseSync
 * 
 */

Handle<Value> ODBCResult::CloseSync(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::CloseSync\n");
  
  HandleScope scope;
  
  OPT_INT_ARG(0, closeOption, SQL_DESTROY);
  
  ODBCResult* result = ObjectWrap::Unwrap<ODBCResult>(args.Holder());
 
  DEBUG_PRINTF("ODBCResult::CloseSync closeOption=%i m_canFreeHandle=%i\n", 
               closeOption, result->m_canFreeHandle);
  
  if (closeOption == SQL_DESTROY && result->m_canFreeHandle) {
    result->Free();
  }
  else if (closeOption == SQL_DESTROY && !result->m_canFreeHandle) {
    //We technically can't free the handle so, we'll SQL_CLOSE
    uv_mutex_lock(&ODBC::g_odbcMutex);
    
    SQLFreeStmt(result->m_hSTMT, SQL_CLOSE);
  
    uv_mutex_unlock(&ODBC::g_odbcMutex);
  }
  else {
    uv_mutex_lock(&ODBC::g_odbcMutex);
    
    SQLFreeStmt(result->m_hSTMT, closeOption);
  
    uv_mutex_unlock(&ODBC::g_odbcMutex);
  }
  
  return scope.Close(True());
}

Handle<Value> ODBCResult::MoreResultsSync(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::MoreResultsSync\n");
  
  HandleScope scope;
  
  ODBCResult* result = ObjectWrap::Unwrap<ODBCResult>(args.Holder());
  
  SQLRETURN ret = SQLMoreResults(result->m_hSTMT);

  if (ret == SQL_ERROR) {
    ThrowException(ODBC::GetSQLError(SQL_HANDLE_STMT, result->m_hSTMT, (char *)"[node-odbc] Error in ODBCResult::MoreResultsSync"));
  }

  return scope.Close(SQL_SUCCEEDED(ret) || ret == SQL_ERROR ? True() : False());
}

/*
 * GetColumnNamesSync
 */

Handle<Value> ODBCResult::GetColumnNamesSync(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::GetColumnNamesSync\n");

  HandleScope scope;
  
  ODBCResult* self = ObjectWrap::Unwrap<ODBCResult>(args.Holder());
  
  Local<Array> cols = Array::New();
  
  if (self->colCount == 0) {
    self->columns = ODBC::GetColumns(self->m_hSTMT, &self->colCount);
  }
  
  for (int i = 0; i < self->colCount; i++) {
    cols->Set(Integer::New(i),
              String::New((const char *) self->columns[i].name));
  }
    
  return scope.Close(cols);
}

/*
 * GetColumnValue
 */

Handle<Value> ODBCResult::GetColumnValue(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::GetColumnValue\n");
  
  HandleScope scope;
  
  ODBCResult* objODBCResult = ObjectWrap::Unwrap<ODBCResult>(args.Holder());
  
  uv_work_t* work_req = (uv_work_t *) (calloc(1, sizeof(uv_work_t)));
  
  get_column_value_work_data* data = (get_column_value_work_data *) calloc(1, sizeof(get_column_value_work_data));
  
  if (args.Length() < 2 || args.Length() > 3 || !args[args.Length() - 1]->IsFunction()) {
    return ThrowException(Exception::TypeError(
      String::New("ODBCResult::GetColumnValue(column, maxBytes, cb): 2 or 3 arguments are required. The last argument must be a callback function.")
      ));
  }

  data->col = args[0]->IntegerValue();
  if (args.Length() == 3) {
    data->bytesRequested = args[1]->IntegerValue();
  } else {
    data->bytesRequested = 0;
  }

  data->cb = Persistent<Function>::New(Local<Function>::Cast(args[args.Length() - 1]));
  data->objResult = objODBCResult;
  work_req->data = data;
  
  uv_queue_work(
    uv_default_loop(), 
    work_req, 
    UV_GetColumnValue, 
    (uv_after_work_cb)UV_AfterGetColumnValue);

  objODBCResult->Ref();

  return scope.Close(Undefined());
}

void ODBCResult::UV_GetColumnValue(uv_work_t* work_req) {
  DEBUG_PRINTF("ODBCResult::UV_GetColumnValue\n");
  
  get_column_value_work_data* data = (get_column_value_work_data*)(work_req->data);
  
  SQLLEN bytesRequested = data->bytesRequested;
  if (bytesRequested <= 0 || bytesRequested > data->objResult->bufferLength)
    bytesRequested = data->objResult->bufferLength;
  
  data->result = SQLColAttribute(
    data->objResult->m_hSTMT,
    data->col + 1,
    SQL_DESC_TYPE,
    NULL,
    0,
    NULL,
    &data->type
    );
  
  if (!SUCCEEDED(data->result))
    return;
  
  switch(data->type) {
    case SQL_INTEGER: case SQL_SMALLINT: case SQL_TINYINT:
      data->type = SQL_C_SLONG;
      break;
    case SQL_NUMERIC: case SQL_DECIMAL: case SQL_BIGINT: case SQL_FLOAT: case SQL_REAL: case SQL_DOUBLE :
      data->type = SQL_C_DOUBLE;
      break;
    case SQL_DATETIME: case SQL_TIMESTAMP:
#if defined(_WIN32)
      data->type = SQL_C_CHAR;
#else
      data->type = SQL_C_TYPE_TIMESTAMP
#endif
    case SQL_BINARY: case SQL_VARBINARY:
      data->type = SQL_C_BINARY;
      break;
    default: // includes SQL_BIT
      data->type = SQL_C_TCHAR;
      break;
  }
  
  data->result = SQLGetData(
    data->objResult->m_hSTMT,
    data->col + 1,
    data->type,
    data->objResult->buffer,
    bytesRequested,
    &data->bytesRead);
}

void ODBCResult::UV_AfterGetColumnValue(uv_work_t* work_req, int status) {
  DEBUG_PRINTF("ODBCResult::UV_AfterGetColumnValue\n");
  
  HandleScope scope;
  
  get_column_value_work_data* data = (get_column_value_work_data*)(work_req->data);
  
  SQLRETURN ret = data->result;

  DEBUG_PRINTF("ODBCResult::UV_AfterGetColumnValue: ret=%i, bytesRead=%i\n", ret, data->bytesRead);

  Handle<Value> args[2];

  if (ret == SQL_ERROR) {
    args[0] = ODBC::GetSQLError(SQL_HANDLE_STMT, data->objResult->m_hSTMT, "[node-odbc] Error in ODBCResult::GetColumnValue");
    args[1] = Null();
  } else if (data->bytesRead == SQL_NULL_DATA || ret == SQL_NO_DATA || data->bytesRead == 0) {
    args[0] = Null();
    args[1] = Null();
  } else {
    args[0] = Null();
    
    switch(data->type) {
      case SQL_C_BINARY: {
        node::Buffer* slowBuffer = node::Buffer::New(data->bytesRead);
        memcpy(node::Buffer::Data(slowBuffer), data->objResult->buffer, data->bytesRead);        
        v8::Local<v8::Object> globalObj = v8::Context::GetCurrent()->Global();
        v8::Local<v8::Function> bufferConstructor = v8::Local<v8::Function>::Cast(globalObj->Get(v8::String::New("Buffer")));
        v8::Handle<v8::Value> constructorArgs[1] = { slowBuffer->handle_ };
        v8::Local<v8::Object> actualBuffer = bufferConstructor->NewInstance(3, constructorArgs);
        args[1] = actualBuffer;
        args[1] = slowBuffer->handle_;

        break;
      }                   
      
      case SQL_C_TCHAR: default:
        #ifdef UNICODE
            args[1] = String::New((uint16_t*) (data->objResult->buffer));
        #else 
            args[1] = String::New((char *) (data->objResult->buffer));
        #endif
        break;
    }
    
  }
  
  TryCatch try_catch;
  
  data->cb->Call(Context::GetCurrent()->Global(), 2, args);
  data->cb.Dispose();

  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }

  free(data);
  free(work_req);
  
  data->objResult->Unref();
}

Handle<Value> ODBCResult::GetColumnValueSync(const Arguments& args) {
  DEBUG_PRINTF("ODBCResult::GetColumnValueSync\n");

  HandleScope scope;

  ODBCResult* objODBCResult = ObjectWrap::Unwrap<ODBCResult>(args.Holder()); 

  DEBUG_PRINTF("ODBCResult::GetColumnValueSync: columns=0x%x, colCount=%i\n", objODBCResult->columns, objODBCResult->colCount);

  Column c; 
  c.index = args[0]->Uint32Value() + 1;
  c.name = (unsigned char*)"(Name not known (ODBCResult::GetColumnValueSync))\0\0";
  c.type = SQL_VARCHAR;
  c.len = 200;
  
  int i = args[0]->Uint32Value();
  if (i < 0 || i >= objODBCResult->colCount)
    return ThrowException(Exception::RangeError(
      String::New("ODBCResult::GetColumnValueSync(): The column index requested is invalid.")
    ));

  return scope.Close(
    ODBC::GetColumnValue(objODBCResult->m_hSTMT, objODBCResult->columns[i], objODBCResult->buffer, objODBCResult->bufferLength)
  );
}