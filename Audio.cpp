/* Audio.cpp
Copyright (c) 2014 by Michael Zahniser

Endless Sky is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.

Endless Sky is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.
*/

#include "Audio.h"

#include "Files.h"
#include "Point.h"
#include "Sound.h"

#include <AL/alut.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std;

namespace {
	class QueueEntry {
	public:
		// Get the coalesced position.
		Point Position() const;
		Point Velocity() const;
		
		void Add(const Point &position, const Point &velocity);
		void Add(const QueueEntry &other);
		
	private:
		Point sum;
		double speed = 0.;
		double weight = 0.;
	};
	
	class Source {
	public:
		Source(const Sound *sound, unsigned source);
		
		void Move(const Point &position, const Point &velocity) const;
		unsigned ID() const;
		const Sound *GetSound() const;
		
	private:
		const Sound *sound = nullptr;
		unsigned source = 0;
	};
	
	void Load();
	string Name(const string &path);
	
	
	mutex audioMutex;
	
	ALCdevice *device = nullptr;
	ALCcontext *context = nullptr;
	double volume = 1.;
	
	thread::id mainThreadID;
	map<const Sound *, QueueEntry> queue;
	map<const Sound *, QueueEntry> deferred;
	
	map<string, Sound> sounds;
	vector<Source> sources;
	vector<unsigned> recycledSources;
	unsigned maxSources = 255;
	
	vector<string> loadQueue;
	thread loadThread;
	
	Point listener;
	Point listenerVelocity;
}



// Begin loading sounds (in a separate thread).
void Audio::Init()
{
	alutInitWithoutContext(nullptr, nullptr);
	device = alcOpenDevice(nullptr);
	if(!device)
		throw runtime_error("Unable to open audio device!");
	
	context = alcCreateContext(device, nullptr);
	if(!context || !alcMakeContextCurrent(context))
		throw runtime_error("Unable to create audio context!");
	
	alListener3f(AL_POSITION, 0., 0., 0.);
	
	mainThreadID = this_thread::get_id();
	
	Files::RecursiveList(Files::Sounds(), &loadQueue);
	if(!loadQueue.empty())
		loadThread = thread(&Load);
}



// Check the progress of loading sounds.
double Audio::Progress()
{
	unique_lock<mutex> lock(audioMutex);
	
	if(loadQueue.empty())
		return 1.;
	
	double done = sounds.size();
	double total = done + loadQueue.size();
	return done / total;
}



// Get or set the volume (between 0 and 1).
double Audio::Volume()
{
	return volume;
}



void Audio::SetVolume(double level)
{
	volume = min(1., max(0., level));
	alListenerf(AL_GAIN, level);
}



// Get a pointer to the named sound. The name is the path relative to the
// "sound/" folder, and without ~ if it's on the end, or the extension.
const Sound *Audio::Get(const std::string &name)
{
	unique_lock<mutex> lock(audioMutex);
	return &sounds[name];
}



// Set the listener's position, and also update any sounds that have been
// added but deferred because they were added from a thread other than the
// main one (the one that called Init()).
void Audio::Update(const Point &listenerPosition, const Point &velocity)
{
	listener = listenerPosition;
	listenerVelocity = velocity;
	
	for(const auto &it : deferred)
		queue[it.first].Add(it.second);
	deferred.clear();
}



// Play the given sound, at full volume.
void Audio::Play(const Sound *sound)
{
	Play(sound, listener, Point());
}



// Play the given sound, as if it is at the given distance from the
// "listener". This will make it softer and change the left / right balance.
void Audio::Play(const Sound *sound, const Point &position, const Point &velocity)
{
	if(!sound || !sound->Buffer() || !volume)
		return;
	
	if(this_thread::get_id() == mainThreadID)
		queue[sound].Add(position - listener, velocity - listenerVelocity);
	else
	{
		unique_lock<mutex> lock(audioMutex);
		deferred[sound].Add(position - listener, velocity - listenerVelocity);
	}
}



// Begin playing all the sounds that have been added since the last time
// this function was called.
void Audio::Step()
{
	// Just to be sure, check we're in the main thread.
	if(this_thread::get_id() != mainThreadID)
		return;
	
	vector<Source> newSources;
	// For each sound that is looping, see if it is going to continue. For other
	// sounds, check if they are done playing.
	for(const Source &source : sources)
	{
		if(source.GetSound()->IsLooping())
		{
			auto it = queue.find(source.GetSound());
			if(it != queue.end())
			{
				source.Move(it->second.Position(), it->second.Velocity());
				newSources.push_back(source);
				queue.erase(it);
			}
			else
			{
				alSourceStop(source.ID());
				recycledSources.push_back(source.ID());
			}
		}
		else
		{
			// Non-looping sounds: check if they're done playing.
			ALint state;
			alGetSourcei(source.ID(), AL_SOURCE_STATE, &state);
			if(state == AL_PLAYING)
				newSources.push_back(source);
			else
				recycledSources.push_back(source.ID());
		}
	}
	newSources.swap(sources);
	
	// Now, what is left in the queue is sounds that want to play, and that do
	// not correspond to an existing source.
	for(const auto &it : queue)
	{
		unsigned source = 0;
		if(recycledSources.empty())
		{
			if(sources.size() >= maxSources)
				break;
			
			alGenSources(1, &source);
			if(!source)
			{
				maxSources = sources.size();
				break;
			}
		}
		else
		{
			source = recycledSources.back();
			recycledSources.pop_back();
		}
		sources.emplace_back(it.first, source);
		sources.back().Move(it.second.Position(), it.second.Velocity());
		alSourcePlay(source);
	}
	queue.clear();
}



// Shut down the audio system (because we're about to quit).
void Audio::Quit()
{
	unique_lock<mutex> lock(audioMutex);
	
	for(const Source &source : sources)
	{
		alSourceStop(source.ID());
		ALuint id = source.ID();
		alDeleteSources(1, &id);
	}
	sources.clear();
	
	for(unsigned id : recycledSources)
		alDeleteSources(1, &id);
	recycledSources.clear();
	
	for(const auto &it : sounds)
	{
		ALuint id = it.second.Buffer();
		alDeleteBuffers(1, &id);
	}
	sounds.clear();
	
	loadThread.join();
	
	alcMakeContextCurrent(nullptr);
	alcDestroyContext(context);
	alcCloseDevice(device);
	alutExit();
}



namespace {
	// Get the coalesced position.
	Point QueueEntry::Position() const
	{
		return weight ? (sum / weight) : sum;
	}
	
	
	
	Point QueueEntry::Velocity() const
	{
		Point pos = Position();
		double length = pos.Length();
		if(!length)
			return pos;
		
		return pos * (speed / length);
	}
	
	
	
	void QueueEntry::Add(const Point &position, const Point &velocity)
	{
		double d = 1. / max(1., position.Dot(position));
		sum += d * position;
		speed += d * sqrt(d) * position.Dot(velocity);
		weight += d;
	}
	
	
	
	void QueueEntry::Add(const QueueEntry &other)
	{
		sum += other.sum;
		speed += other.speed;
		weight += other.weight;
	}
	
	
	
	Source::Source(const Sound *sound, unsigned source)
		: sound(sound), source(source)
	{
		alSourcef(source, AL_PITCH, 1);
		alSourcef(source, AL_GAIN, 1);
		alSourcei(source, AL_LOOPING, sound->IsLooping());
		alSourcei(source, AL_BUFFER, sound->Buffer());
	}
	
	
	
	void Source::Move(const Point &position, const Point &velocity) const
	{
		alSource3f(source, AL_POSITION, position.X() * .001, position.Y() * .001, 0.f);
		alSource3f(source, AL_VELOCITY, velocity.X() * .001, velocity.Y() * .001, 0.f);
	}
	
	
	
	unsigned Source::ID() const
	{
		return source;
	}
	
	
	
	const Sound *Source::GetSound() const
	{
		return sound;
	}
	
	
	
	void Load()
	{
		string path;
		while(true)
		{
			string name = Name(path);
			if(!name.empty())
				sounds[name].Load(path);
			
			{
				// If Load() was called, the file list was not initially empty.
				unique_lock<mutex> lock(audioMutex);
				loadQueue.pop_back();
				if(loadQueue.empty())
					return;
				
				path = loadQueue.back();
			}
		}
	}
	
	
	
	string Name(const string &path)
	{
		if(path.length() < 4 || path.compare(path.length() - 4, 4, ".wav"))
			return string();
		
		size_t start = path.rfind("sounds/");
		if(start == string::npos)
			return string();
		start += 7;
		
		size_t end = path.length() - 4;
		if(path[end - 1] == '~')
			--end;
		
		return path.substr(start, end - start);
	}
}
