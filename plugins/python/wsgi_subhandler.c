#include "uwsgi_python.h"

extern struct uwsgi_server uwsgi;
extern struct uwsgi_python up;
extern struct uwsgi_plugin python_plugin;
extern PyTypeObject uwsgi_InputType;

/*
this is a hack for supporting non-file object passed to wsgi.file_wrapper
*/
static void uwsgi_python_consume_file_wrapper_read(struct wsgi_request *wsgi_req, PyObject *pychunk) {
	PyObject *read_method_args = NULL;
	PyObject *read_method = PyObject_GetAttrString(pychunk, "read");
	if (wsgi_req->sendfile_fd_chunk > 0) {
        	read_method_args = PyTuple_New(1);
		PyTuple_SetItem(read_method_args, 0, PyInt_FromLong(wsgi_req->sendfile_fd_chunk));
	}
	else {
        	read_method_args = PyTuple_New(0);
	}
	for(;;) {
        	PyObject *read_method_output = PyEval_CallObject(read_method, read_method_args);
                if (PyErr_Occurred()) {
                	uwsgi_manage_exception(wsgi_req, 0);
			break;
                }
		if (!read_method_output) break;
               	if (PyString_Check(read_method_output)) {
                     	char *content = PyString_AsString(read_method_output);
                        size_t content_len = PyString_Size(read_method_output);
			if (content_len == 0) {
                        	Py_DECREF(read_method_output);
				break;
			}
                        UWSGI_RELEASE_GIL
                        uwsgi_response_write_body_do(wsgi_req, content, content_len);
                        UWSGI_GET_GIL
                }
		Py_DECREF(read_method_output);
		if (wsgi_req->sendfile_fd_chunk == 0) break;
	}
        Py_DECREF(read_method_args);
        Py_DECREF(read_method);
}

void *uwsgi_request_subhandler_wsgi(struct wsgi_request *wsgi_req, struct uwsgi_app *wi) {


	PyObject *zero;
	int i;
	PyObject *pydictkey, *pydictvalue;
	char *path_info;

        for (i = 0; i < wsgi_req->var_cnt; i += 2) {
#ifdef UWSGI_DEBUG
                uwsgi_debug("%.*s: %.*s\n", wsgi_req->hvec[i].iov_len, wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i+1].iov_len, wsgi_req->hvec[i+1].iov_base);
#endif
#ifdef PYTHREE
                pydictkey = PyUnicode_DecodeLatin1(wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i].iov_len, NULL);
                pydictvalue = PyUnicode_DecodeLatin1(wsgi_req->hvec[i + 1].iov_base, wsgi_req->hvec[i + 1].iov_len, NULL);
#else
                pydictkey = PyString_FromStringAndSize(wsgi_req->hvec[i].iov_base, wsgi_req->hvec[i].iov_len);
                pydictvalue = PyString_FromStringAndSize(wsgi_req->hvec[i + 1].iov_base, wsgi_req->hvec[i + 1].iov_len);
#endif

#ifdef UWSGI_DEBUG
		uwsgi_log("%p %d %p %d\n", pydictkey, wsgi_req->hvec[i].iov_len, pydictvalue, wsgi_req->hvec[i + 1].iov_len);
#endif
                PyDict_SetItem(wsgi_req->async_environ, pydictkey, pydictvalue);
                Py_DECREF(pydictkey);
		Py_DECREF(pydictvalue);
        }

        if (wsgi_req->uh->modifier1 == UWSGI_MODIFIER_MANAGE_PATH_INFO) {
                wsgi_req->uh->modifier1 = python_plugin.modifier1;
                pydictkey = PyDict_GetItemString(wsgi_req->async_environ, "SCRIPT_NAME");
                if (pydictkey) {
                        if (PyString_Check(pydictkey)) {
                                pydictvalue = PyDict_GetItemString(wsgi_req->async_environ, "PATH_INFO");
                                if (pydictvalue) {
                                        if (PyString_Check(pydictvalue)) {
                                                path_info = PyString_AsString(pydictvalue);
                                                PyDict_SetItemString(wsgi_req->async_environ, "PATH_INFO", PyString_FromString(path_info + PyString_Size(pydictkey)));
                                        }
                                }
                        }
                }
        }


        // create wsgi.input custom object
        wsgi_req->async_input = (PyObject *) PyObject_New(uwsgi_Input, &uwsgi_InputType);
        ((uwsgi_Input*)wsgi_req->async_input)->wsgi_req = wsgi_req;


        PyDict_SetItemString(wsgi_req->async_environ, "wsgi.input", wsgi_req->async_input);

	PyDict_SetItemString(wsgi_req->async_environ, "wsgi.file_wrapper", wi->sendfile);

	if (uwsgi.async > 1) {
		PyDict_SetItemString(wsgi_req->async_environ, "x-wsgiorg.fdevent.readable", wi->eventfd_read);
		PyDict_SetItemString(wsgi_req->async_environ, "x-wsgiorg.fdevent.writable", wi->eventfd_write);
		PyDict_SetItemString(wsgi_req->async_environ, "x-wsgiorg.fdevent.timeout", Py_None);
	}

	PyDict_SetItemString(wsgi_req->async_environ, "wsgi.version", wi->gateway_version);

	PyDict_SetItemString(wsgi_req->async_environ, "wsgi.errors", wi->error);

	PyDict_SetItemString(wsgi_req->async_environ, "wsgi.run_once", Py_False);



	if (uwsgi.threads > 1) {
		PyDict_SetItemString(wsgi_req->async_environ, "wsgi.multithread", Py_True);
	}
	else {
		PyDict_SetItemString(wsgi_req->async_environ, "wsgi.multithread", Py_False);
	}
	if (uwsgi.numproc == 1) {
		PyDict_SetItemString(wsgi_req->async_environ, "wsgi.multiprocess", Py_False);
	}
	else {
		PyDict_SetItemString(wsgi_req->async_environ, "wsgi.multiprocess", Py_True);
	}


	if (wsgi_req->scheme_len > 0) {
		zero = UWSGI_PYFROMSTRINGSIZE(wsgi_req->scheme, wsgi_req->scheme_len);
	}
	else if (wsgi_req->https_len > 0) {
		if (!strncasecmp(wsgi_req->https, "on", 2) || wsgi_req->https[0] == '1') {
			zero = UWSGI_PYFROMSTRING("https");
		}
		else {
			zero = UWSGI_PYFROMSTRING("http");
		}
	}
	else {
		zero = UWSGI_PYFROMSTRING("http");
	}
	PyDict_SetItemString(wsgi_req->async_environ, "wsgi.url_scheme", zero);
	Py_DECREF(zero);

	wsgi_req->async_app = wi->callable;

	// export .env only in non-threaded mode
	if (uwsgi.threads < 2) {
		PyDict_SetItemString(up.embedded_dict, "env", wsgi_req->async_environ);
	}

	PyDict_SetItemString(wsgi_req->async_environ, "uwsgi.version", wi->uwsgi_version);

	if (uwsgi.cores > 1) {
		zero = PyInt_FromLong(wsgi_req->async_id);
		PyDict_SetItemString(wsgi_req->async_environ, "uwsgi.core", zero);
		Py_DECREF(zero);
	}

	PyDict_SetItemString(wsgi_req->async_environ, "uwsgi.node", wi->uwsgi_node);

	// call
	PyTuple_SetItem(wsgi_req->async_args, 0, wsgi_req->async_environ);
	return python_call(wsgi_req->async_app, wsgi_req->async_args, uwsgi.catch_exceptions, wsgi_req);
}

int uwsgi_response_subhandler_wsgi(struct wsgi_request *wsgi_req) {

	PyObject *pychunk = NULL;
	const char* chunk = NULL;
	Py_ssize_t chunk_size = 0;

	// return or yield ?
	PyObject *async_result = wsgi_req->async_result;

	if (PyObject_AsCharBuffer(async_result,	&chunk, &chunk_size)<0) {
		PyErr_Clear();

		// is one element list
		if (PyList_Check(async_result) &&
			PyList_GET_SIZE(async_result)==1) {
			PyObject *elem = PyList_GET_ITEM(async_result, 0);
			if (PyObject_AsCharBuffer(elem, &chunk, &chunk_size)<0) {
				PyErr_Clear();
			}
		}
	}

	if(chunk) {
		UWSGI_RELEASE_GIL
		uwsgi_response_write_body_do(wsgi_req, (char*)chunk, chunk_size);
		UWSGI_GET_GIL
		uwsgi_py_check_write_errors {
			uwsgi_py_write_exception(wsgi_req);
		}
		goto clear;
	}

	if (wsgi_req->sendfile_obj == async_result) {
		if (wsgi_req->sendfile_fd >= 0) {
			UWSGI_RELEASE_GIL
			uwsgi_response_sendfile_do(wsgi_req, wsgi_req->sendfile_fd, 0, 0);
			UWSGI_GET_GIL
		}
		// we do not have an iterable, check for read() method
		else if (PyObject_HasAttrString(async_result, "read")) {
			uwsgi_python_consume_file_wrapper_read(wsgi_req, async_result);
		}
		uwsgi_py_check_write_errors {
			uwsgi_py_write_exception(wsgi_req);
		}
		goto clear;
	}

	// ok its a yield
	if (!wsgi_req->async_placeholder) {
		wsgi_req->async_placeholder = PyObject_GetIter(async_result);
		if (!wsgi_req->async_placeholder) {
			goto exception;
		}
		if (uwsgi.async > 1) {
			return UWSGI_AGAIN;
		}
	}

	pychunk = PyIter_Next(wsgi_req->async_placeholder);

	if (!pychunk) {
exception:
		if (PyErr_Occurred()) { 
			uwsgi_manage_exception(wsgi_req, uwsgi.catch_exceptions);
		}	
		goto clear;
	}

	if (PyObject_AsCharBuffer(pychunk, &chunk, &chunk_size)==0) {
		UWSGI_RELEASE_GIL
		uwsgi_response_write_body_do(wsgi_req, (char*)chunk, chunk_size);
		UWSGI_GET_GIL
		uwsgi_py_check_write_errors {
			uwsgi_py_write_exception(wsgi_req);
			Py_DECREF(pychunk);
			goto clear;
		}
	}
	else {
		PyErr_Clear();

		if (wsgi_req->sendfile_obj == pychunk) {
			if (wsgi_req->sendfile_fd >= 0) {
				UWSGI_RELEASE_GIL
				uwsgi_response_sendfile_do(wsgi_req, wsgi_req->sendfile_fd, 0, 0);
				UWSGI_GET_GIL
			}
			// we do not have an iterable, check for read() method
			else if (PyObject_HasAttrString(pychunk, "read")) {
				uwsgi_python_consume_file_wrapper_read(wsgi_req, pychunk);
			}

			uwsgi_py_check_write_errors {
				uwsgi_py_write_exception(wsgi_req);
				Py_DECREF(pychunk);
				goto clear;
			}
		}
	}

	Py_DECREF(pychunk);
	return UWSGI_AGAIN;

clear:

	if (wsgi_req->sendfile_fd != -1) {
		Py_DECREF((PyObject *)wsgi_req->async_sendfile);
	}

	if (wsgi_req->async_placeholder) {
		// CALL close() ALWAYS if we are working with an iterator !!!
		if (PyObject_HasAttrString((PyObject *)wsgi_req->async_result, "close")) {
                        PyObject *close_method = PyObject_GetAttrString((PyObject *)wsgi_req->async_result, "close");
                        PyObject *close_method_args = PyTuple_New(0);
#ifdef UWSGI_DEBUG
                        uwsgi_log("calling close() for %.*s %p %p\n", wsgi_req->uri_len, wsgi_req->uri, close_method, close_method_args);
#endif
                        PyObject *close_method_output = PyEval_CallObject(close_method, close_method_args);
                        if (PyErr_Occurred()) {
                                uwsgi_manage_exception(wsgi_req, 0);
                        }
                        Py_DECREF(close_method_args);
                        Py_XDECREF(close_method_output);
                        Py_DECREF(close_method);
                }
		Py_DECREF((PyObject *)wsgi_req->async_placeholder);
	}

	Py_DECREF((PyObject *)wsgi_req->async_result);
	PyErr_Clear();

	return UWSGI_OK;
}


