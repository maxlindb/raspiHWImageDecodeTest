// Copyright (C) 2013 Matt Ownby
// You are free to use this for educational/non-commercial purposes only
// http://my-cool-projects.blogspot.com

#ifdef USE_OPENMAX

#include "Event.h"

OMXEvent::OMXEvent(OMXEventData ev) : m_ev(ev)
{ }

OMXEventData *OMXEvent::ToEvent()
{
	return &m_ev;
}

EmptyBufferDoneData *OMXEvent::ToEmpty()
{
	return NULL;
}

FillBufferDoneData *OMXEvent::ToFill()
{
	return NULL;
}

EmptyBufferDoneEvent::EmptyBufferDoneEvent(EmptyBufferDoneData ev) : m_ev(ev)
{
}

OMXEventData *EmptyBufferDoneEvent::ToEvent()
{
	return NULL;
}

EmptyBufferDoneData *EmptyBufferDoneEvent::ToEmpty()
{
	return &m_ev;
}

FillBufferDoneData *EmptyBufferDoneEvent::ToFill()
{
	return NULL;
}

FillBufferDoneEvent::FillBufferDoneEvent(FillBufferDoneData ev) : m_ev(ev)
{
}

OMXEventData *FillBufferDoneEvent::ToEvent()
{
	return NULL;
}

EmptyBufferDoneData *FillBufferDoneEvent::ToEmpty()
{
	return NULL;
}

FillBufferDoneData *FillBufferDoneEvent::ToFill()
{
	return &m_ev;
}

#endif // USE_OPENMAX
