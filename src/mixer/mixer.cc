/*
 * Channel Mixer Plugin for Audacious
 * Copyright 2011-2012 John Lindgren and Michał Lipski
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

/* TODO: implement more surround converters */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <libaudcore/audstrings.h>
#include <libaudcore/i18n.h>
#include <libaudcore/runtime.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>

class ChannelMixer : public EffectPlugin
{
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Channel Mixer"),
        PACKAGE,
        about,
        & prefs
    };

    /* order #2: must be before crossfade */
    constexpr ChannelMixer () : EffectPlugin (info, 2, false) {}

    bool init ();
    void cleanup ();

    void start (int & channels, int & rate);
    Index<float> & process (Index<float> & data);
};

EXPORT ChannelMixer aud_plugin_instance;

typedef Index<float> & (* Converter) (Index<float> & data);

typedef struct
{
    int in;
    int out;
    int pos;
} MATRIX_REF;

static Index<float> mixer_buf;
static Index<float> matrix_buf;
static Index<MATRIX_REF> matrix_map;

static Index<float> & mono_to_stereo (Index<float> & data)
{
    int frames = data.len ();
    mixer_buf.resize (2 * frames);

    float * get = data.begin ();
    float * set = mixer_buf.begin ();

    while (frames --)
    {
        float val = * get ++;
        * set ++ = val;
        * set ++ = val;
    }

    return mixer_buf;
}

static Index<float> & stereo_to_mono (Index<float> & data)
{
    int frames = data.len () / 2;
    mixer_buf.resize (frames);

    float * get = data.begin ();
    float * set = mixer_buf.begin ();

    while (frames --)
    {
        float val = * get ++;
        val += * get ++;
        * set ++ = val / 2;
    }

    return mixer_buf;
}

static Index<float> & quadro_to_stereo (Index<float> & data)
{
    int frames = data.len () / 4;
    mixer_buf.resize (2 * frames);

    float * get = data.begin ();
    float * set = mixer_buf.begin ();

    while (frames --)
    {
        float front_left  = * get ++;
        float front_right = * get ++;
        float back_left   = * get ++;
        float back_right  = * get ++;
        * set ++ = front_left + (back_left * 0.7);
        * set ++ = front_right + (back_right * 0.7);
    }

    return mixer_buf;
}

static Index<float> & surround_5p1_to_stereo (Index<float> & data)
{
    int frames = data.len () / 6;
    mixer_buf.resize (2 * frames);

    float * get = data.begin ();
    float * set = mixer_buf.begin ();

    while (frames --)
    {
        float front_left  = * get ++;
        float front_right = * get ++;
        float center = * get ++;
        float lfe    = * get ++;
        float rear_left   = * get ++;
        float rear_right  = * get ++;
        * set ++ = front_left + (center * 0.5) + (lfe * 0.5) + (rear_left * 0.5);
        * set ++ = front_right + (center * 0.5) + (lfe * 0.5) + (rear_right * 0.5);
    }

    return mixer_buf;
}

/* 5 channels case. Quad + center channel */
static Index<float> & quadro_5_to_stereo (Index<float> & data)
{
    int frames = data.len () / 5;
    mixer_buf.resize (2 * frames);

    float * get = data.begin ();
    float * set = mixer_buf.begin ();

    while (frames --)
    {
        float front_left  = * get ++;
        float front_right = * get ++;
        float center = * get ++;
        float rear_left   = * get ++;
        float rear_right  = * get ++;
        * set ++ = front_left + (center * 0.5) + rear_left;
        * set ++ = front_right + (center * 0.5) + rear_right;
    }

    return mixer_buf;
}

static int input_channels, output_channels;
static MATRIX_REF * current_matrix;

static Index<float> & matrix_convert (Index<float> & data)
{
    int frames = data.len () / input_channels;
    mixer_buf.resize (output_channels * frames);

    float * get = data.begin ();
    float * set = mixer_buf.begin ();
    float * mx = matrix_buf.begin () + current_matrix->pos;

    while (frames --)
    {
        for (int y = 0; y < output_channels; y ++)
        {
            * (set + y) = 0;
            for (int x = 0; x < input_channels; x ++)
                * (set + y) += * (get + x) * * (mx + y * input_channels + x);
        }
        
        get += input_channels;
        set += output_channels;
    }

    return mixer_buf;
}

static Converter get_converter (int in, int out)
{
    if (current_matrix != nullptr)
        return matrix_convert;
    
    if (in == 1 && out == 2)
        return mono_to_stereo;
    if (in == 2 && out == 1)
        return stereo_to_mono;
    if (in == 4 && out == 2)
        return quadro_to_stereo;
    if (in == 5 && out == 2)
        return quadro_5_to_stereo;
    if (in == 6 && out == 2)
        return surround_5p1_to_stereo;

    return nullptr;
}

static bool load_matrix(const char * matrix_path) {
    int nChr, nLine = 0, mPos = 0;
    char szLine[256];
    const char *e = 0;
    float * mx;
    MATRIX_REF * m = nullptr;
    
    AUDINFO ("Loading mixing matrix definitions from %s\n", matrix_path);
    FILE *fMap = fopen (matrix_path, "r");
    
    if (fMap != NULL) {
        matrix_map.clear ();
        matrix_buf.clear ();
        
        int y = 0;
        while (fgets (szLine, sizeof (szLine), fMap) && !e)
        {
            nLine++;
            if (szLine[0] == '#' || szLine[0] == '\n') continue;
            
            if (m == nullptr)
            {
                matrix_map.insert (-1, 1);
                m = matrix_map.end () - 1;
                if (sscanf(szLine, "%d:%d", &m->in, &m->out) == 2) 
                {
                    //AUDINFO ("%d:%d\n", m->in, m->out);
                    if (m->in < 0 || m->out < 0 || m->in > AUD_MAX_CHANNELS || m->out > AUD_MAX_CHANNELS)
                        e = "Channel count out of bounds";
                    else {
                        m->pos = mPos;
                        matrix_buf.insert (-1, m->in * m->out);
                        mPos += m->in * m->out;
                        mx = matrix_buf.begin () + m->pos;
                    }
                }
                else
                    e = "Could not parse definition";
            }
            else
            {
                int cPos = 0;
                
                for (int x = 0; x < m->in; x++)
                {
                    float * val = mx + y * m->in + x;
                    * val = 0;
                    nChr = 0;
                    sscanf (szLine + cPos, "%f%n", val, &nChr);
                    cPos += nChr;
                    //AUDINFO ("%d %d %f %d\n", y, x, * val, cPos);
                }
                
                y++;
                
                if (y == m->out)
                {
                    m = nullptr;
                    y = 0;
                }
            }
        }
        
        fclose(fMap);
        
        if (y) e = "Premature end of file";
        
        if (e)
        {
            AUDERR ("%s @%d\n", e, nLine);
            return false;
        }
    }
    else
    {
        AUDERR ("%s\n", strerror(errno));
        return false;
    }
    
    int n = matrix_map.len ();
    m = matrix_map.begin ();
    while (n > 0)
    {
        if (m->in == input_channels && m->out == output_channels)
        {
            current_matrix = m;
            
            AUDINFO ("Using mixing matrix for %d to %d channels\n", current_matrix->in, current_matrix->out);
            szLine[sizeof(szLine) - 1] = 0;
            
            nChr = sprintf(szLine, "%s", "          ");
            for (int x = 0; x < input_channels; x ++)
                nChr += snprintf (szLine + nChr, sizeof(szLine) - nChr - 1, "  Input %2d", x + 1);
            
            AUDINFO ("%s\n", szLine);
            
            mx = matrix_buf.begin () + current_matrix->pos;
            for (int y = 0; y < output_channels; y ++)
            {
                nChr = 0;
                for (int x = 0; x < input_channels; x ++)
                    nChr += snprintf (szLine + nChr, sizeof(szLine) - nChr - 1, "  %f", * (mx + y * input_channels + x));
                
                AUDINFO ("Output %2d:%s\n", y + 1, szLine);
            }
            
            return true;
        }
        
        m ++;
        n --;
    }
    
    return false;
}

void ChannelMixer::start (int & channels, int & rate)
{
    input_channels = channels;
    output_channels = aud_get_int ("mixer", "channels");

    current_matrix = nullptr;
    String matrix_file = aud_get_str ("mixer", "matrix_file");
    
    if (matrix_file[0])
    {
        if (! load_matrix (matrix_file[0] == '/' || (matrix_file[0] == '.' && matrix_file[1] == '/') ? matrix_file
        : filename_build ({aud_get_path (AudPath::UserDir), "mixer", matrix_file})))
        {
            AUDERR ("Mixing matrix of %d to %d channels requested but not found.\n",
            input_channels, output_channels);
        }
    }
    
    if (current_matrix == nullptr)
    {
        if (input_channels == output_channels)
            return;

        if (! get_converter (input_channels, output_channels))
        {
            AUDERR ("Converting %d to %d channels is not implemented.\n",
            input_channels, output_channels);
            return;
        }
    }

    channels = output_channels;
}

Index<float> & ChannelMixer::process (Index<float> & data)
{
    //if (input_channels == output_channels)
    //    return data;

    Converter converter = get_converter (input_channels, output_channels);
    if (converter)
        return converter (data);

    return data;
}

const char * const ChannelMixer::defaults[] = {
 "channels", "2",
 "matrix_file", "",
  nullptr};

bool ChannelMixer::init ()
{
    aud_config_set_defaults ("mixer", defaults);
    return true;
}

void ChannelMixer::cleanup ()
{
    mixer_buf.clear ();
    matrix_map.clear ();
    matrix_buf.clear ();
}

const char ChannelMixer::about[] =
 N_("Channel Mixer Plugin for Audacious\n"
    "Copyright 2011-2012 John Lindgren and Michał Lipski");

const PreferencesWidget ChannelMixer::widgets[] = {
    WidgetLabel (N_("<b>Channel Mixer</b>")),
    WidgetSpin (N_("Output channels:"),
        WidgetInt ("mixer", "channels"),
        {1, AUD_MAX_CHANNELS, 1}),
    WidgetLabel (N_("Matrix definition file (optional):")),
    WidgetEntry (nullptr,
        WidgetString ("mixer", "matrix_file"))
};

const PluginPreferences ChannelMixer::prefs = {{widgets}};
