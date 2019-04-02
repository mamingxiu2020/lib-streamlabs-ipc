/******************************************************************************
    Copyright (C) 2016-2019 by Streamlabs (General Workings Inc)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

******************************************************************************/

#include "ipc-client.hpp"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <functional>
#include <iterator>
#include "../include/error.hpp"
#include "../include/tags.hpp"
#include "windows/semaphore.hpp"

#ifdef _WIN32
#include <windows.h>
#include <Objbase.h>
#endif

using namespace std::placeholders;

call_return_t g_fn   = NULL;
void*         g_data = NULL;
int64_t       g_cbid = NULL;

void ipc::client::worker() {
	os::error ec = os::error::Success;
	std::vector<ipc::value> proc_rval;

	while (m_socket->is_connected() && !m_watcher.stop) {
		if (!m_rop || !m_rop->is_valid()) {
			m_watcher.rbuf.resize(sizeof(ipc_size_t));
			ec = m_socket->read(m_watcher.rbuf.data(), m_watcher.rbuf.size(), m_rop, std::bind(&client::read_callback_init, this, _1, _2));
			if (ec != os::error::Pending && ec != os::error::Success) {
				if (ec == os::error::Disconnected) {
					break;
				} else {
					throw std::exception("Unexpected error.");
				}
			}
		}

		if (!m_wop || !m_wop->is_valid()) {
			if (m_watcher.m_write_queue.size() > 0) {
				std::vector<char>& fbuf = m_watcher.m_write_queue.front();
				ipc::make_sendable(m_watcher.wbuf, fbuf);
				ec = m_socket->write(
				    m_watcher.wbuf.data(),
				    m_watcher.wbuf.size(),
				    m_wop,
				    std::bind(&client::write_callback, this, _1, _2));
				if (ec != os::error::Pending && ec != os::error::Success) {
					if (ec == os::error::Disconnected) {
						break;
					} else {
						throw std::exception("Unexpected error.");
					}
				}
				m_watcher.m_write_queue.pop();
			}
		}

		os::waitable* waits[]    = {m_rop.get(), m_wop.get()};
		size_t        wait_index = -1;
		for (size_t idx = 0; idx < 2; idx++) {
			if (waits[idx] != nullptr) {
				if (waits[idx]->wait(std::chrono::milliseconds(0)) == os::error::Success) {
					wait_index = idx;
					break;
				}
			}
		}
		if (wait_index == -1) {
			os::error code = os::waitable::wait_any(waits, 2, wait_index, std::chrono::milliseconds(20));
			if (code == os::error::TimedOut) {
				continue;
			} else if (code == os::error::Disconnected) {
				break;
			} else if (code == os::error::Error) {
				throw std::exception("Error");
			}
		}
	}

	// Call any remaining callbacks.
	proc_rval.resize(1);
	proc_rval[0].type = ipc::type::Null;
	proc_rval[0].value_str = "Lost IPC Connection";

	{ // ToDo: Figure out better way of registering functions, perhaps even a way to have "events" across a IPC connection.
		std::unique_lock<std::mutex> ulock(m_lock);
		for (auto& cb : m_cb) {
			cb.second.first(cb.second.second, proc_rval);
		}

		m_cb.clear();
	}

	if (!m_socket->is_connected()) {
		exit(1);
	}
}

void ipc::client::read_callback_init(os::error ec, size_t size) {
	os::error ec2 = os::error::Success;

	m_rop->invalidate();

	if (ec == os::error::Success || ec == os::error::MoreData) {
		ipc_size_t n_size = read_size(m_watcher.rbuf);

		if (n_size != 0) {
			m_watcher.rbuf.resize(n_size);
			ec2 = m_socket->read(m_watcher.rbuf.data(), m_watcher.rbuf.size(), m_rop, std::bind(&client::read_callback_msg, this, _1, _2));
			if (ec2 != os::error::Pending && ec2 != os::error::Success) {
				if (ec2 == os::error::Disconnected) {
					return;
				} else {
					throw std::exception("Unexpected error.");
				}
			}
		}
	}
}

void ipc::client::read_callback_msg(os::error ec, size_t size) {
	std::pair<call_return_t, void*> cb;
	ipc::message::function_reply fnc_reply_msg;

	m_rop->invalidate();

	bool is_fnc_call = ipc::message::is_function_call(m_watcher.rbuf, 0);
	if (is_fnc_call) {
		handle_fnc_call();
	} else {
		handle_fnc_reply();
	}
}

ipc::client::client(std::string socketPath) {
	m_socket = std::make_unique<os::windows::named_pipe>(os::open_only, socketPath, os::windows::pipe_read_mode::Byte);

	m_watcher.stop   = false;
	m_watcher.worker = std::thread(std::bind(&client::worker, this));
}

ipc::client::~client() {
	m_watcher.stop = true;
	if (m_watcher.worker.joinable()) {
		m_watcher.worker.join();
	}
	m_socket = nullptr;
}

bool ipc::client::cancel(int64_t const& id) {
	std::unique_lock<std::mutex> ulock(m_lock);
	return m_cb.erase(id) != 0;
}

bool ipc::client::call(const std::string& cname, const std::string& fname, std::vector<ipc::value> args, call_return_t fn, void* data, int64_t& cbid) {
	static std::mutex mtx;
	static uint64_t timestamp = 0;
	os::error ec;
	std::shared_ptr<os::async_op> write_op;
	ipc::message::function_call fnc_call_msg;
	std::vector<char> outbuf;

	if (!m_socket)
		return false;

	{
		std::unique_lock<std::mutex> ulock(mtx);
		timestamp++;
		fnc_call_msg.uid = ipc::value(timestamp);
	}

	// Set	
	fnc_call_msg.class_name = ipc::value(cname);
	fnc_call_msg.function_name = ipc::value(fname);
	fnc_call_msg.arguments = std::move(args);

	// Serialize
	std::vector<char> buf(fnc_call_msg.size());
	try {
		fnc_call_msg.serialize(buf, 0);
	} catch (std::exception& e) {
		ipc::log("(write) %8llu: Failed to serialize, error %s.", fnc_call_msg.uid.value_union.ui64, e.what());
		throw e;
	}

	if (fn != nullptr) {
		std::unique_lock<std::mutex> ulock(m_lock);
		m_cb.insert(std::make_pair(fnc_call_msg.uid.value_union.ui64, std::make_pair(fn, data)));
		cbid = fnc_call_msg.uid.value_union.ui64;
	}

	ipc::make_sendable(outbuf, buf);
	ec = m_socket->write(outbuf.data(), outbuf.size(), write_op, nullptr);
	if (ec != os::error::Success && ec != os::error::Pending) {
		cancel(cbid);
		//write_op->cancel();
		return false;
	}

	ec = write_op->wait();
	if (ec != os::error::Success) {
		cancel(cbid);
		write_op->cancel();
		return false;
	}

	return true;
}

std::vector<ipc::value> ipc::client::call_synchronous_helper(const std::string & cname, const std::string & fname, const std::vector<ipc::value>& args) {
	// Set up call reference data.
	struct CallData {
		std::shared_ptr<os::windows::semaphore> sgn = std::make_shared<os::windows::semaphore>();
		bool called = false;
		std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();

		std::vector<ipc::value> values;
	} cd;

	auto cb = [](const void* data, const std::vector<ipc::value>& rval) {
		CallData& cd = const_cast<CallData&>(*static_cast<const CallData*>(data));

		// This copies the data off of the reply thread to the main thread.
		cd.values.reserve(rval.size());
		std::copy(rval.begin(), rval.end(), std::back_inserter(cd.values));

		cd.called = true;
		cd.sgn->signal();
	};

	int64_t cbid = 0;
	bool success = call(cname, fname, std::move(args), cb, &cd, cbid);
	if (!success) {
		return {};
	}

	cd.sgn->wait();
	if (!cd.called) {
		cancel(cbid);
		return {};
	}
	return std::move(cd.values);
}

bool ipc::client::register_collection(std::shared_ptr<ipc::collection> cls)
{
	if (m_classes.count(cls->get_name()) > 0)
		return false;

	m_classes.insert(std::make_pair(cls->get_name(), cls));
	return true;
}

void ipc::client::handle_fnc_call()
{
	std::vector<ipc::value>      proc_rval;
	std::string                  proc_error;
	ipc::message::function_call  fnc_call_msg;
	ipc::message::function_reply fnc_reply_msg;
	std::vector<char>            write_buffer;
	bool                         success = false;

	try {
		fnc_call_msg.deserialize(m_watcher.rbuf, 0);
	} catch (std::exception e) {
		ipc::log("????????: Deserialization of Function Call message failed with error %s.", e.what());
		return;
	}

	// Execute
	proc_rval.resize(0);
	try {
		success = server_call_function(
		    -1, // Server
		    fnc_call_msg.class_name.value_str,
		    fnc_call_msg.function_name.value_str,
		    fnc_call_msg.arguments,
		    proc_rval,
		    proc_error);
	} catch (std::exception e) {
		ipc::log(
		    "%8llu: Unexpected exception during client call, error %s.", fnc_call_msg.uid.value_union.ui64, e.what());
		throw e;
	}
}

void ipc::client::handle_fnc_reply()
{
	std::pair<call_return_t, void*> cb;
	ipc::message::function_reply    fnc_reply_msg;

	m_rop->invalidate();

	try {
		fnc_reply_msg.deserialize(m_watcher.rbuf, 0);
	} catch (std::exception e) {
		ipc::log("Deserialize failed with error %s.", e.what());
		throw e;
	}

	// Find the callback function.
	std::unique_lock<std::mutex> ulock(m_lock);
	auto                         cb2 = m_cb.find(fnc_reply_msg.uid.value_union.ui64);
	if (cb2 == m_cb.end()) {
		return;
	}
	cb = cb2->second;

	// Decode return values or errors.
	if (fnc_reply_msg.error.value_str.size() > 0) {
		fnc_reply_msg.values.resize(1);
		fnc_reply_msg.values.at(0).type      = ipc::type::Null;
		fnc_reply_msg.values.at(0).value_str = fnc_reply_msg.error.value_str;
	}

	// Call Callback
	cb.first(cb.second, fnc_reply_msg.values);

	// Remove cb entry
	/// ToDo: Figure out better way of registering functions, perhaps even a way to have "events" across a IPC connection.
	m_cb.erase(fnc_reply_msg.uid.value_union.ui64);
}

bool ipc::client::server_call_function(
    int64_t                  cid,
    const std::string&       cname,
    const std::string&       fname,
    std::vector<ipc::value>& args,
    std::vector<ipc::value>& rval,
    std::string&             errormsg)
{
	if (m_classes.count(cname) == 0) {
		errormsg = "Class '" + cname + "' is not registered.";
		return false;
	}
	auto cls = m_classes.at(cname);

	auto fnc = cls->get_function(fname, args);
	if (!fnc) {
		errormsg = "Function '" + fname + "' not found in class '" + cname + "'.";
		return false;
	}

	fnc->call(cid, args, rval);

	return true;
}

void ipc::client::write_callback(os::error ec, size_t size)
{
	m_wop->invalidate();
	m_rop->invalidate();
}