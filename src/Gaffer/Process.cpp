//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2016, Image Engine Design Inc. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//
//      * Redistributions of source code must retain the above
//        copyright notice, this list of conditions and the following
//        disclaimer.
//
//      * Redistributions in binary form must reproduce the above
//        copyright notice, this list of conditions and the following
//        disclaimer in the documentation and/or other materials provided with
//        the distribution.
//
//      * Neither the name of John Haddon nor the names of
//        any other contributors to this software may be used to endorse or
//        promote products derived from this software without specific prior
//        written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////

#include "Gaffer/Process.h"

#include "Gaffer/BackgroundTask.h"
#include "Gaffer/Context.h"
#include "Gaffer/Monitor.h"
#include "Gaffer/Node.h"
#include "Gaffer/Plug.h"

#include "IECore/Canceller.h"

#include "boost/container/flat_set.hpp"

#include "tbb/enumerable_thread_specific.h"

using namespace Gaffer;

namespace
{

typedef boost::container::flat_set<Monitor *> Monitors;
Monitors g_activeMonitors;

// Used by `handleException()/emitError()` to track the original (most upstream)
// source of an error.
// \todo This assumes that each error propagates on a single thread, but when
// TBB tasks are accounted for, this is not true. A better approach might be to
// wrap exceptions in a ProcessException class in `handleException()`, storing
// the error source in the ProcessException instance. There are a couple of obstacles
// to this :
//
// - We wouldn't want a ProcessException to be stored inside ValuePlug's LRUCache,
//   because the error source might have ceased to exist when it is rethrown on the
//   next cache access. We might be able to deal with this by unwrapping and rewrapping
//   the exception at the appropriate points in ValuePlug.
// - Exception types are not preserved when a C++ exception enters Python and then
//   gets translated back into C++. We would at least need to preserve ProcessException's
//   exactly, if not other types.
//
// Still, it seems like the most promising approach, and ProcessException could have
// other nice features, like including the plug name in the error message (which again,
// we wouldn't want stored inside the ValuePlug cache).
tbb::enumerable_thread_specific<const Plug *> g_errorSource( nullptr );

} // namespace

Process::Process( const IECore::InternedString &type, const Plug *plug, const Plug *downstream )
	:	m_type( type ), m_plug( plug ), m_downstream( downstream ? downstream : plug )
{
	IECore::Canceller::check( context()->canceller() );
	m_parent = m_threadState->m_process;
	m_threadState->m_process = this;

	for( Monitors::const_iterator it = g_activeMonitors.begin(), eIt = g_activeMonitors.end(); it != eIt; ++it )
	{
		(*it)->processStarted( this );
	}
}

Process::~Process()
{
	for( Monitors::const_iterator it = g_activeMonitors.begin(), eIt = g_activeMonitors.end(); it != eIt; ++it )
	{
		(*it)->processFinished( this );
	}
	if( !parent() )
	{
		g_errorSource.local() = nullptr;
	}
}

const Process *Process::current()
{
	return ThreadState::current().m_process;
}

void Process::handleException()
{
	try
	{
		// Rethrow the current exception
		// so we can examine it.
		throw;
	}
	catch( const IECore::Cancelled &e )
	{
		// Process is just being cancelled. No need
		// to report via `emitError()`.
		throw;
	}
	catch( const std::exception &e )
	{
		if( !g_errorSource.local() )
		{
			g_errorSource.local() = plug();
		}
		emitError( e.what() );
		throw;
	}
	catch( ... )
	{
		if( !g_errorSource.local() )
		{
			g_errorSource.local() = plug();
		}
		emitError( "Unknown error" );
		throw;
	}
}

void Process::emitError( const std::string &error ) const
{
	const Plug *plug = m_downstream;
	while( plug )
	{
		if( plug->direction() == Plug::Out )
		{
			if( const Node *node = plug->node() )
			{
				node->errorSignal()( plug, g_errorSource.local(), error );
			}
		}
		plug = plug != m_plug ? plug->getInput() : nullptr;
	}
}

void Process::registerMonitor( Monitor *monitor )
{
	// Because `g_activeMonitors` is global state, we can't modify it
	// while other threads are creating processes. So we cancel all BackgroundTasks
	// to make sure that is not the case. This is needed by the TransformTool,
	// which attaches a monitor temporarily at a point when the viewer is likely
	// to be updating asynchronously. Without this workaround, at best the TransformTool
	// ends up monitoring processes it doesn't want to, and at worst we get
	// crashes.
	//
	// This is a bit of a hack, but it represents progress towards an improved Monitor API.
	// When making a monitor active, ideally it should only be active for processes launched from
	// _the current thread_, and any processes that those processes launch (potentially on other
	// threads). In other words, we only want to monitor process trees that originate from
	// inside the monitor's active scope. To do this properly we need to be able to track
	// `Process::parent()` accurately when a process uses TBB to launch child processes on
	// other threads. So for now we use this poor man's version whereby we at least prevent
	// background tasks from being monitored inadvertently.
	BackgroundTask::cancelAllTasks();
	g_activeMonitors.insert( monitor );
}

void Process::deregisterMonitor( Monitor *monitor )
{
	BackgroundTask::cancelAllTasks();
	g_activeMonitors.erase( monitor );
}

bool Process::monitorRegistered( const Monitor *monitor )
{
	return g_activeMonitors.find( const_cast<Monitor *>( monitor ) ) != g_activeMonitors.end();
}

