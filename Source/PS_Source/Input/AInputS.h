/*
  Copyright (C) 2006-2011 Nasca Octavian Paul
  Author: Nasca Octavian Paul

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License 
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License (version 2) for more details.

  You should have received a copy of the GNU General Public License (version 2)
  along with this program; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
*/

#pragma once

#include <string>

#include "../JuceLibraryCode/JuceHeader.h"

//#include <audiofile.h>
#include "InputS.h"

inline double ramp(int64_t pos, int64_t totallen, int64_t rampinlen, int64_t rampoutlen)
{
	if (totallen < rampinlen + rampoutlen)
		return 1.0;
	if (pos < rampinlen)
		return 1.0 / rampinlen*pos;
	if (pos >= totallen - rampoutlen)
		return 1.0 - 1.0 / rampoutlen*(pos - totallen + rampoutlen);
	return 1.0;
}

class AInputS final : public InputS
{
public:
    AInputS(AudioFormatManager* mana) : m_manager(mana)
	{
		m_readbuf.setSize(2, 65536*2);
		m_readbuf.clear();
		m_crossfadebuf.setSize(2, 44100);
		m_crossfadebuf.clear();
        PlayRangeEndCallback=[](AInputS*){};
	}
    ~AInputS() {}
	void setAudioBuffer(AudioBuffer<float>* buf, int samplerate, int len)
	{
		m_afreader = nullptr;
		m_using_memory_buffer = true;
		m_readbuf = *buf;
		info.nchannels = buf->getNumChannels();
		info.nsamples = len;
		info.samplerate = samplerate;
		m_currentsample = 0;
		m_crossfadebuf.setSize(info.nchannels, m_crossfadebuf.getNumSamples());
		m_cached_file_range = { 0,len };
		updateXFadeCache();
	}
	bool openAudioFile(File file) override
    {
		m_silenceoutputted = 0;
		AudioFormatReader* reader = m_manager->createReaderFor(file);
        if (reader)
        {
			m_using_memory_buffer = false;
			m_afreader = std::unique_ptr<AudioFormatReader>(reader);
            m_currentsample = 0;
            info.samplerate = (int)m_afreader->sampleRate;
            info.nchannels = m_afreader->numChannels;
            info.nsamples = m_afreader->lengthInSamples;
			if (m_readbuf.getNumChannels() < info.nchannels)
			{
				m_readbuf.setSize(info.nchannels, m_readbuf.getNumSamples());
				m_crossfadebuf.setSize(info.nchannels, m_crossfadebuf.getNumSamples());
			}
			m_readbuf.clear();
            return true;
        }
        return false;
    }
	void close() override
    {
		m_afreader = nullptr;
		m_currentsample = 0;
		info.nchannels = 0;
		info.nsamples = 0;
		info.samplerate = 0;
    }
	int readNextBlock(AudioBuffer<float>& abuf, int nsmps, int numchans) override
	{
		if (m_afreader == nullptr && m_using_memory_buffer == false)
			return 0;
		int inchans = 0;
		if (m_afreader)
			inchans = m_afreader->numChannels;
		else inchans = m_readbuf.getNumChannels();
		int64_t subsect_t0 = (int64_t)(m_activerange.getStart()*info.nsamples);
		int64_t subsect_t1 = (int64_t)(m_activerange.getEnd()*info.nsamples);
		int64_t subsectlen = subsect_t1 - subsect_t0;
		int xfadelen = m_xfadelen;
		if (xfadelen >= subsectlen)
			xfadelen = int(subsectlen - 2);
		auto getSampleLambda=[this](int64_t pos, int ch)
		{
			if (m_cached_file_range.contains(pos))
                return m_readbuf.getSample(ch, int(pos - m_cached_file_range.getStart()));
            else
            {
                Range<int64_t> activerange((int64_t)(m_activerange.getStart()*info.nsamples), 
					(int64_t)(m_activerange.getEnd()*info.nsamples+1));
                Range<int64_t> possiblerange(pos, pos + m_readbuf.getNumSamples() + 0);
                m_cached_file_range = activerange.getIntersectionWith(possiblerange);
                m_afreader->read(&m_readbuf, 0, (int)m_cached_file_range.getLength(), pos, true, true);
                return m_readbuf.getSample(ch, int(pos - m_cached_file_range.getStart()));
            }
        };
		auto getCrossFadedSampleLambda=[this,&getSampleLambda](int64_t playpos, int chan, int64_t subt0, int64_t subt1, int xfadelen)
		{
			if (m_loop_enabled == false && playpos >= subt1)
				return 0.0f;
			if (playpos >= subt0 && playpos <= subt1 - xfadelen)
				return getSampleLambda(playpos, chan);
			if (playpos > (subt1 - xfadelen) && playpos<subt1)
			{
				int64_t fadeindex = playpos - subt1 + xfadelen;
				double fadeoutgain = 1.0 - (1.0 / (xfadelen - 0))*fadeindex;
				float s0 = (float)(getSampleLambda(playpos, chan)*fadeoutgain);
				double fadeingain = (1.0 / (xfadelen - 0))*fadeindex;
                int64_t playpos2 = playpos - subt1 + xfadelen;
                jassert(playpos2>=0 && playpos2<=xfadelen);
                float s1 = (float)(m_crossfadebuf.getSample(chan, (int)playpos2)*fadeingain);
				return s0 + s1;
			}
			++m_silenceoutputted;
			return 0.0f;
		};
		float** smps = abuf.getArrayOfWritePointers();
		int readinc = 1;
		if (m_reverseplay)
			readinc = -1;
		for (int i = 0; i < nsmps; ++i)
		{
			
			if (inchans == 1 && numchans > 0)
			{
				float sig = getCrossFadedSampleLambda(m_currentsample, 0, subsect_t0, subsect_t1, xfadelen);
				for (int j = 0; j < numchans; ++j)
				{
					smps[j][i] = sig;
				}
			}
			else if (inchans > 1 && numchans > 1)
			{
				for (int j = 0; j < numchans; ++j)
				{
					int inchantouse = j % inchans;
					smps[j][i] = getCrossFadedSampleLambda(m_currentsample, inchantouse, subsect_t0, subsect_t1,xfadelen);
				}

			}
			m_currentsample += readinc;
			if (m_loop_enabled == true)
			{
				if (m_reverseplay == false && m_currentsample >= subsect_t1)
				{
					m_currentsample = subsect_t0+xfadelen;
					++m_loopcount;
				} 
				else if (m_reverseplay == true && m_currentsample < subsect_t0)
				{
					m_currentsample = subsect_t1 - 1;
				}
			} else
            {
				if (m_reverseplay == false && m_currentsample == subsect_t1)
					PlayRangeEndCallback(this);
				else if (m_reverseplay == true && m_currentsample == subsect_t0)
					PlayRangeEndCallback(this);
            }
		}
		
		return nsmps;
	}
	void seek(double pos) override //0=start,1.0=end
    {
		if (m_using_memory_buffer == true)
		{
			jassert(m_readbuf.getNumSamples() > 0 && m_afreader==nullptr);
			m_loopcount = 0;
			m_silenceoutputted = 0;
			m_cache_misses = 0;
			m_currentsample = (int64_t)(pos*m_readbuf.getNumSamples());
			m_currentsample = jlimit<int64_t>(0, m_readbuf.getNumSamples(), m_currentsample);
			m_cached_file_range = { 0,m_readbuf.getNumSamples() };
			return;
		}
		if (m_afreader==nullptr)
            return;
		m_loopcount = 0;
		m_silenceoutputted = 0;
		m_cache_misses = 0;
		m_currentsample = (int64_t)(pos*m_afreader->lengthInSamples);
		m_currentsample = jlimit<int64_t>(0, m_afreader->lengthInSamples, m_currentsample);
		//Logger::writeToLog("Seeking to " + String(m_currentsample));
		//if (m_cached_file_range.contains(info.currentsample)==false)
		m_cached_file_range = Range<int64_t>();
		updateXFadeCache();
        //m_cached_crossfade_range = Range<int64_t>();
	}
	std::pair<Range<double>,Range<double>> getCachedRangesNormalized()
	{
		if (m_afreader == nullptr)
			return {};
		return { { jmap<double>((double)m_cached_file_range.getStart(),0,(double)info.nsamples,0.0,1.0),
			jmap<double>((double)m_cached_file_range.getEnd(), 0, (double)info.nsamples, 0.0, 1.0) },
			{ jmap<double>((double)m_cached_crossfade_range.getStart(),0,(double)info.nsamples,0.0,1.0),
			jmap<double>((double)m_cached_crossfade_range.getEnd(), 0, (double)info.nsamples, 0.0, 1.0) } };
	}
	int getNumCacheMisses() { return m_cache_misses; }
    void updateXFadeCache()
    {
        if (m_xfadelen>m_crossfadebuf.getNumSamples())
            m_crossfadebuf.setSize(info.nchannels,m_xfadelen);
        if (m_afreader != nullptr && m_using_memory_buffer == false)
			m_afreader->read(&m_crossfadebuf, 0, m_xfadelen, (int64_t)(m_activerange.getStart()*info.nsamples), true, true);
		if (m_afreader == nullptr && m_using_memory_buffer == true)
			m_crossfadebuf.copyFrom(0, 0, m_readbuf, 0, (int64_t)(m_activerange.getStart()*info.nsamples), m_xfadelen);
		m_cached_crossfade_range =
            Range<int64_t>((int64_t)(m_activerange.getStart()*info.nsamples),(int64_t)(m_activerange.getStart()*info.nsamples+m_xfadelen));
    }
	void setActiveRange(Range<double> rng) override
	{
		if (rng.getEnd() < rng.getStart())
			rng = { 0.0,1.0 };
		if (rng.isEmpty())
			rng = { 0.0,1.0 };
		m_activerange = rng;
		m_loopcount = 0;
        updateXFadeCache();
    }
	void setLoopEnabled(bool b) override
	{
		m_loop_enabled = b;
		m_loopcount = 0;
        updateXFadeCache();
	}
    void setXFadeLenSeconds(double len)
    {
        if (info.samplerate==0)
            return;
        len = jlimit(0.0,1.0,len);
        int temp = (int)(len*info.samplerate);
        if (m_xfadelen!=temp)
        {
            m_xfadelen = temp;
            updateXFadeCache();
        }
    }
	Range<int64_t> getActiveRangeFrames()
	{
		if (info.nsamples == 0)
			return Range<int64_t>();
		return Range<int64_t>((int64_t)(m_activerange.getStart()*info.nsamples), (int64_t)(m_activerange.getEnd()*info.nsamples));
	}
	void setReversePlay(bool b)
	{
		m_reverseplay = b;
	}
	bool isReversed() { return m_reverseplay; }
	int64_t getLoopCount() { return m_loopcount; }
	
private:
	std::function<void(AInputS*)> PlayRangeEndCallback;
	std::unique_ptr<AudioFormatReader> m_afreader;
	AudioBuffer<float> m_readbuf;
	AudioBuffer<float> m_crossfadebuf;
	Range<int64_t> m_cached_file_range;
	Range<int64_t> m_cached_crossfade_range;
	int m_cache_misses = 0;
	int m_fade_in = 512;
	int m_fade_out = 512;
	int m_xfadelen = 0;
	bool m_reverseplay = false;
	int64_t m_loopcount = 0;
	bool m_using_memory_buffer = false;
	AudioFormatManager* m_manager = nullptr;
};
