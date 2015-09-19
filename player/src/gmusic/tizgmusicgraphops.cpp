/**
 * Copyright (C) 2011-2015 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   tizgmusicgraphops.cpp
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  Google Music client graph implementation
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <algorithm>

#include <boost/bind.hpp>
#include <boost/make_shared.hpp>

#include <OMX_Core.h>
#include <OMX_Component.h>
#include <OMX_TizoniaExt.h>
#include <tizplatform.h>

#include "tizgraphutil.hpp"
#include "tizprobe.hpp"
#include "tizgraph.hpp"
#include "tizgmusicconfig.hpp"
#include "tizgmusicgraphops.hpp"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.play.graph.gmusic.ops"
#endif

namespace graph = tiz::graph;

namespace
{
  void copy_omx_string (OMX_U8 *p_dest, const std::string &omx_string)
  {
    const size_t len = omx_string.length ();
    const size_t to_copy = MIN (len, OMX_MAX_STRINGNAME_SIZE - 1);
    assert (NULL != p_dest);
    memcpy (p_dest, omx_string.c_str (), to_copy);
    p_dest[to_copy] = '\0';
  }
}

//
// gmusicops
//
graph::gmusicops::gmusicops (graph *p_graph,
                             const omx_comp_name_lst_t &comp_lst,
                             const omx_comp_role_lst_t &role_lst)
  : tiz::graph::ops (p_graph, comp_lst, role_lst),
    encoding_ (OMX_AUDIO_CodingAutoDetect)
{
}

void graph::gmusicops::do_enable_auto_detection (const int handle_id,
                                                 const int port_id)
{
  tizgmusicconfig_ptr_t gmusic_config
      = boost::dynamic_pointer_cast< gmusicconfig >(config_);
  assert (gmusic_config);
  tiz::graph::ops::do_enable_auto_detection (handle_id, port_id);
  tiz::graph::util::dump_graph_info ("Google Play Music",
                                     "Connecting",
                                     gmusic_config->get_user_name ().c_str ());
}

void graph::gmusicops::do_disable_ports ()
{
  OMX_U32 gmusic_source_port = 0;
  G_OPS_BAIL_IF_ERROR (util::disable_port (handles_[0], gmusic_source_port),
                       "Unable to disable gmusic source's output port.");
  clear_expected_port_transitions ();
  add_expected_port_transition (handles_[0], gmusic_source_port,
                                OMX_CommandPortDisable);
}

void graph::gmusicops::do_configure_source ()
{
  tizgmusicconfig_ptr_t gmusic_config
      = boost::dynamic_pointer_cast< gmusicconfig >(config_);
  assert (gmusic_config);

  G_OPS_BAIL_IF_ERROR (
      set_gmusic_user_and_device_id (
          handles_[0], gmusic_config->get_user_name (),
          gmusic_config->get_user_pass (), gmusic_config->get_device_id ()),
      "Unable to set OMX_TizoniaIndexParamAudioGmusicSession");

  G_OPS_BAIL_IF_ERROR (
      set_gmusic_playlist (handles_[0], playlist_->get_current_uri ()),
      "Unable to set OMX_TizoniaIndexParamAudioGmusicPlaylist");
}

void graph::gmusicops::do_load ()
{
  assert (!comp_lst_.empty ());
  assert (!role_lst_.empty ());

  // At this point we are going to instantiate the remaining component in the
  // graph, the audio decoder and the pcm renderer. The gmusic source is already
  // instantiated and in
  // Executing state.

  assert (comp_lst_.size () == 1);
  assert (handles_.size () == 1);

  G_OPS_BAIL_IF_ERROR (
      get_encoding_type_from_gmusic_source (),
      "Unable to retrieve the audio encoding from the gmusic source.");

  omx_comp_name_lst_t comp_list;
  omx_comp_role_lst_t role_list;

  comp_list.push_back ("OMX.Aratelia.audio_decoder.mp3");
  role_list.push_back ("audio_decoder.mp3");

  comp_list.push_back (tiz::graph::util::get_default_pcm_renderer ());
  role_list.push_back ("audio_renderer.pcm");

  tiz::graph::cbackhandler &cbacks = get_cback_handler ();
  G_OPS_BAIL_IF_ERROR (
      util::instantiate_comp_list (comp_list, handles_, h2n_, &(cbacks),
                                   cbacks.get_omx_cbacks ()),
      "Unable to instantiate the component list.");

  // Now add the new components to the base class lists
  comp_lst_.insert (comp_lst_.begin (), comp_list.begin (), comp_list.end ());
  role_lst_.insert (role_lst_.begin (), role_list.begin (), role_list.end ());
}

void graph::gmusicops::do_configure ()
{
  if (last_op_succeeded ())
  {
    G_OPS_BAIL_IF_ERROR (apply_pcm_codec_info_from_decoder (),
                         "Unable to set OMX_IndexParamAudioPcm");
  }
}

void graph::gmusicops::do_omx_loaded2idle ()
{
  if (last_op_succeeded ())
  {
    // Transition the decoder and the renderer components to Idle
    omx_comp_handle_lst_t decoder_and_renderer_handles;
    decoder_and_renderer_handles.push_back (handles_[1]);  // the decoder
    decoder_and_renderer_handles.push_back (handles_[2]);  // the renderer
    G_OPS_BAIL_IF_ERROR (
        util::transition_all (decoder_and_renderer_handles, OMX_StateIdle,
                              OMX_StateLoaded),
        "Unable to transition deoder and renderer from Loaded->Idle");
    clear_expected_transitions ();
    add_expected_transition (handles_[1], OMX_StateIdle);
    add_expected_transition (handles_[2], OMX_StateIdle);
  }
}

void graph::gmusicops::do_omx_idle2exe ()
{
  if (last_op_succeeded ())
  {
    // Transition the decoder and the renderer components to Exe
    omx_comp_handle_lst_t decoder_and_renderer_handles;
    decoder_and_renderer_handles.push_back (handles_[1]);  // the decoder
    decoder_and_renderer_handles.push_back (handles_[2]);  // the renderer
    G_OPS_BAIL_IF_ERROR (
        util::transition_all (decoder_and_renderer_handles, OMX_StateExecuting,
                              OMX_StateIdle),
        "Unable to transition decoder and renderer from Idle->Exe");
    clear_expected_transitions ();
    add_expected_transition (handles_[1], OMX_StateExecuting);
    add_expected_transition (handles_[2], OMX_StateExecuting);
  }
}

void graph::gmusicops::do_reconfigure_tunnel (const int tunnel_id)
{
  if (last_op_succeeded ())
  {
    if (0 == tunnel_id)
    {
      do_reconfigure_first_tunnel ();
    }
    else if (1 == tunnel_id)
    {
      do_reconfigure_second_tunnel ();
    }
    else
    {
      assert (0);
    }
  }
}

void graph::gmusicops::do_skip ()
{
  if (last_op_succeeded () && 0 != jump_)
  {
    assert (!handles_.empty ());
    G_OPS_BAIL_IF_ERROR (util::apply_playlist_jump (handles_[0], jump_),
                         "Unable to skip in playlist");
    // Reset the jump value, to its default value
    jump_ = SKIP_DEFAULT_VALUE;
  }
}

void graph::gmusicops::do_retrieve_metadata ()
{
  dump_stream_metadata ();
}

// TODO: Move this implementation to the base class (and remove also from
// httpservops)
OMX_ERRORTYPE
graph::gmusicops::transition_tunnel (
    const int tunnel_id, const OMX_COMMANDTYPE to_disabled_or_enabled)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  assert (0 == tunnel_id || 1 == tunnel_id);
  assert (to_disabled_or_enabled == OMX_CommandPortDisable
          || to_disabled_or_enabled == OMX_CommandPortEnable);

  if (to_disabled_or_enabled == OMX_CommandPortDisable)
  {
    rc = tiz::graph::util::disable_tunnel (handles_, tunnel_id);
  }
  else
  {
    rc = tiz::graph::util::enable_tunnel (handles_, tunnel_id);
  }

  if (OMX_ErrorNone == rc && 0 == tunnel_id)
  {
    const int gmusic_source_index = 0;
    const int gmusic_source_output_port = 0;
    add_expected_port_transition (handles_[gmusic_source_index],
                                  gmusic_source_output_port,
                                  to_disabled_or_enabled);
    const int decoder_index = 1;
    const int decoder_input_port = 0;
    add_expected_port_transition (handles_[decoder_index], decoder_input_port,
                                  to_disabled_or_enabled);
  }
  else if (OMX_ErrorNone == rc && 1 == tunnel_id)
  {
    const int decoder_index = 1;
    const int decoder_output_port = 1;
    add_expected_port_transition (handles_[decoder_index], decoder_output_port,
                                  to_disabled_or_enabled);
    const int renderer_index = 2;
    const int renderer_input_port = 0;
    add_expected_port_transition (handles_[renderer_index], renderer_input_port,
                                  to_disabled_or_enabled);
  }
  return rc;
}

bool graph::gmusicops::probe_stream_hook ()
{
  return true;
}

void graph::gmusicops::dump_stream_metadata ()
{
  OMX_U32 index = 0;
  const int gmusic_index = 0;
  // Extract metadata from the gmusic source
  while (OMX_ErrorNone == dump_metadata_item (index++, gmusic_index))
  {
  };

  // Now extract metadata from the decoder
  const int decoder_index = 1;
  index = 0;
  while (OMX_ErrorNone == dump_metadata_item (index++, decoder_index))
  {
  };

  OMX_GetParameter (handles_[2], OMX_IndexParamAudioPcm, &renderer_pcmtype_);

  TIZ_PRINTF_YEL (
      "   %ld Ch, %g KHz, %lu:%s:%s \n", renderer_pcmtype_.nChannels,
      ((float)renderer_pcmtype_.nSamplingRate) / 1000,
      renderer_pcmtype_.nBitPerSample,
      renderer_pcmtype_.eNumData == OMX_NumericalDataSigned ? "s" : "u",
      renderer_pcmtype_.eEndian == OMX_EndianBig ? "b" : "l");
}

OMX_ERRORTYPE graph::gmusicops::dump_metadata_item (const OMX_U32 index,
                                                    const int comp_index)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  OMX_CONFIG_METADATAITEMTYPE *p_meta = NULL;
  size_t metadata_len = 0;
  size_t value_len = 0;

  value_len = OMX_MAX_STRINGNAME_SIZE;
  metadata_len = sizeof(OMX_CONFIG_METADATAITEMTYPE) + value_len;

  if (NULL == (p_meta = (OMX_CONFIG_METADATAITEMTYPE *)tiz_mem_calloc (
                   1, metadata_len)))
  {
    rc = OMX_ErrorInsufficientResources;
  }
  else
  {
    p_meta->nSize = metadata_len;
    p_meta->nVersion.nVersion = OMX_VERSION;
    p_meta->eScopeMode = OMX_MetadataScopeAllLevels;
    p_meta->nScopeSpecifier = 0;
    p_meta->nMetadataItemIndex = index;
    p_meta->eSearchMode = OMX_MetadataSearchValueSizeByIndex;
    p_meta->eKeyCharset = OMX_MetadataCharsetASCII;
    p_meta->eValueCharset = OMX_MetadataCharsetASCII;
    p_meta->nKeySizeUsed = 0;
    p_meta->nValue[0] = '\0';
    p_meta->nValueMaxSize = OMX_MAX_STRINGNAME_SIZE;
    p_meta->nValueSizeUsed = 0;

    rc = OMX_GetConfig (handles_[comp_index], OMX_IndexConfigMetadataItem,
                        p_meta);
    if (OMX_ErrorNone == rc)
    {
      TIZ_PRINTF_CYN ("   %s%s : %s\n", index ? "  " : "", p_meta->nKey,
                      p_meta->nValue);
    }

    tiz_mem_free (p_meta);
    p_meta = NULL;
  }
  return rc;
}

OMX_ERRORTYPE graph::gmusicops::get_encoding_type_from_gmusic_source ()
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  const OMX_U32 port_id = 0;
  TIZ_INIT_OMX_PORT_STRUCT (port_def, port_id);
  tiz_check_omx_err (
      OMX_GetParameter (handles_[0], OMX_IndexParamPortDefinition, &port_def));
  encoding_ = port_def.format.audio.eEncoding;
  return OMX_ErrorNone;
}

OMX_ERRORTYPE
graph::gmusicops::apply_pcm_codec_info_from_decoder ()
{
  OMX_U32 channels = 2;
  OMX_U32 sampling_rate = 44100;
  std::string encoding_str;

  tiz_check_omx_err (get_channels_and_rate_from_decoder (
      channels, sampling_rate, encoding_str));
  tiz_check_omx_err (set_channels_and_rate_on_renderer (channels, sampling_rate,
                                                        encoding_str));
  return OMX_ErrorNone;
}

OMX_ERRORTYPE
graph::gmusicops::get_channels_and_rate_from_decoder (
    OMX_U32 &channels, OMX_U32 &sampling_rate, std::string &encoding_str) const
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  const OMX_HANDLETYPE handle = handles_[1];  // mp3 decoder's handle
  const OMX_U32 port_id = 1;                  // mp3 decoder's output port

  switch (encoding_)
  {
    case OMX_AUDIO_CodingMP3:
    {
      encoding_str = "mp3";
      rc = tiz::graph::util::
          get_channels_and_rate_from_audio_port_v2< OMX_AUDIO_PARAM_PCMMODETYPE >(
              handle, port_id, OMX_IndexParamAudioPcm, channels, sampling_rate);
    }
    break;
    default:
    {
      assert (0);
    }
    break;
  };

  TIZ_LOG (TIZ_PRIORITY_TRACE, "outcome = [%s]", tiz_err_to_str (rc));

  return rc;
}

OMX_ERRORTYPE
graph::gmusicops::set_channels_and_rate_on_renderer (
    const OMX_U32 channels, const OMX_U32 sampling_rate,
    const std::string encoding_str)
{
  const OMX_HANDLETYPE handle = handles_[2];  // renderer's handle
  const OMX_U32 port_id = 0;                  // renderer's input port

  TIZ_LOG (TIZ_PRIORITY_TRACE, "channels = [%d] sampling_rate = [%d]", channels,
           sampling_rate);

  // Retrieve the pcm settings from the renderer component
  TIZ_INIT_OMX_PORT_STRUCT (renderer_pcmtype_, port_id);
  tiz_check_omx_err (
      OMX_GetParameter (handle, OMX_IndexParamAudioPcm, &renderer_pcmtype_));

  // Now assign the actual settings to the pcmtype structure
  renderer_pcmtype_.nChannels = channels;
  renderer_pcmtype_.nSamplingRate = sampling_rate;
  renderer_pcmtype_.eNumData = OMX_NumericalDataSigned;
  renderer_pcmtype_.eEndian
      = (encoding_ == OMX_AUDIO_CodingMP3 ? OMX_EndianBig : OMX_EndianLittle);

  // Set the new pcm settings
  tiz_check_omx_err (
      OMX_SetParameter (handle, OMX_IndexParamAudioPcm, &renderer_pcmtype_));

  std::string coding_type_str ("Google Play Music");
  tiz::graph::util::dump_graph_info (coding_type_str.c_str (),
                                     "Connected",
                                     playlist_->get_current_uri ().c_str ());
//   dump_stream_metadata ();

  return OMX_ErrorNone;
}

OMX_ERRORTYPE
graph::gmusicops::set_gmusic_user_and_device_id (const OMX_HANDLETYPE handle,
                                                 const std::string &user,
                                                 const std::string &pass,
                                                 const std::string &device_id)
{
  // Set the Google Music user and pass
  OMX_TIZONIA_AUDIO_PARAM_GMUSICSESSIONTYPE sessiontype;
  TIZ_INIT_OMX_STRUCT (sessiontype);
  tiz_check_omx_err (OMX_GetParameter (
      handle,
      static_cast< OMX_INDEXTYPE >(OMX_TizoniaIndexParamAudioGmusicSession),
      &sessiontype));
  copy_omx_string (sessiontype.cUserName, user);
  copy_omx_string (sessiontype.cUserPassword, pass);
  copy_omx_string (sessiontype.cDeviceId, device_id);
  return OMX_SetParameter (handle, static_cast< OMX_INDEXTYPE >(
                                       OMX_TizoniaIndexParamAudioGmusicSession),
                           &sessiontype);
}

OMX_ERRORTYPE
graph::gmusicops::set_gmusic_playlist (const OMX_HANDLETYPE handle,
                                       const std::string &playlist)
{
  // Set the Google Music playlist
  OMX_TIZONIA_AUDIO_PARAM_GMUSICPLAYLISTTYPE playlisttype;
  TIZ_INIT_OMX_STRUCT (playlisttype);
  tiz_check_omx_err (OMX_GetParameter (
      handle,
      static_cast< OMX_INDEXTYPE >(OMX_TizoniaIndexParamAudioGmusicPlaylist),
      &playlisttype));
  copy_omx_string (playlisttype.cPlaylistName, playlist);

  tizgmusicconfig_ptr_t gmusic_config
    = boost::dynamic_pointer_cast< gmusicconfig >(config_);
  assert (gmusic_config);

  playlisttype.ePlaylistType = gmusic_config->get_playlist_type ();
  playlisttype.bShuffle = playlist_->shuffle () ? OMX_TRUE : OMX_FALSE;
  playlisttype.bAllAccessSearch = gmusic_config->is_all_access_search () ? OMX_TRUE : OMX_FALSE;

  return OMX_SetParameter (
      handle,
      static_cast< OMX_INDEXTYPE >(OMX_TizoniaIndexParamAudioGmusicPlaylist),
      &playlisttype);
}

bool graph::gmusicops::is_fatal_error (const OMX_ERRORTYPE error) const
{
  bool rc = false;
  TIZ_LOG (TIZ_PRIORITY_ERROR, "[%s] ", tiz_err_to_str (error));
  if (error == error_code_)
  {
    // if this error is already being handled, then ignore it.
    rc = false;
  }
  else
  {
    rc |= tiz::graph::ops::is_fatal_error (error);
    rc |= (OMX_ErrorContentURIError == error);
  }
  return rc;
}

void graph::gmusicops::do_record_fatal_error (const OMX_HANDLETYPE handle,
                                              const OMX_ERRORTYPE error,
                                              const OMX_U32 port)
{
  tiz::graph::ops::do_record_fatal_error (handle, error, port);
  if (error == OMX_ErrorContentURIError)
  {
    error_msg_.append ("\n [Playlist not found]");
  }
}

void graph::gmusicops::do_reconfigure_first_tunnel ()
{
  // Retrieve the mp3 settings from the gmusic source component
  OMX_AUDIO_PARAM_MP3TYPE gmusic_mp3type;
  const OMX_U32 gmusic_port_id = 0;
  TIZ_INIT_OMX_PORT_STRUCT (gmusic_mp3type, gmusic_port_id);
  G_OPS_BAIL_IF_ERROR (
      OMX_GetParameter (handles_[0], OMX_IndexParamAudioMp3, &gmusic_mp3type),
      "Unable to retrieve the MP3 settings from the gmusic source");

  // Retrieve the mp3 settings from the decoder component
  OMX_AUDIO_PARAM_MP3TYPE decoder_mp3type;
  const OMX_U32 decoder_port_id = 0;
  TIZ_INIT_OMX_PORT_STRUCT (decoder_mp3type, decoder_port_id);
  G_OPS_BAIL_IF_ERROR (
      OMX_GetParameter (handles_[1], OMX_IndexParamAudioMp3, &decoder_mp3type),
      "Unable to retrieve the MP3 settings from the audio decoder");

  // Now assign the current settings to the decoder structure
  decoder_mp3type.nChannels = gmusic_mp3type.nChannels;
  decoder_mp3type.nSampleRate = gmusic_mp3type.nSampleRate;

  // Set the new mp3 settings
  G_OPS_BAIL_IF_ERROR (
      OMX_SetParameter (handles_[1], OMX_IndexParamAudioMp3, &decoder_mp3type),
      "Unable to set the MP3 settings on the audio decoder");
}

void graph::gmusicops::do_reconfigure_second_tunnel ()
{
  // Retrieve the pcm settings from the decoder component
  OMX_AUDIO_PARAM_PCMMODETYPE decoder_pcmtype;
  const OMX_U32 decoder_port_id = 1;
  TIZ_INIT_OMX_PORT_STRUCT (decoder_pcmtype, decoder_port_id);
  G_OPS_BAIL_IF_ERROR (
      OMX_GetParameter (handles_[1], OMX_IndexParamAudioPcm, &decoder_pcmtype),
      "Unable to retrieve the PCM settings from the Mp3 decoder");

  // Retrieve the pcm settings from the renderer component
  OMX_AUDIO_PARAM_PCMMODETYPE renderer_pcmtype;
  const OMX_U32 renderer_port_id = 0;
  TIZ_INIT_OMX_PORT_STRUCT (renderer_pcmtype, renderer_port_id);
  G_OPS_BAIL_IF_ERROR (
      OMX_GetParameter (handles_[2], OMX_IndexParamAudioPcm, &renderer_pcmtype),
      "Unable to retrieve the PCM settings from the pcm renderer");

  // Now assign the current settings to the renderer structure
  renderer_pcmtype.nChannels = decoder_pcmtype.nChannels;
  renderer_pcmtype.nSamplingRate = decoder_pcmtype.nSamplingRate;

  // Set the new pcm settings
  G_OPS_BAIL_IF_ERROR (
      OMX_SetParameter (handles_[2], OMX_IndexParamAudioPcm, &renderer_pcmtype),
      "Unable to set the PCM settings on the audio renderer");

  TIZ_PRINTF_YEL (
      "   %ld Ch, %g KHz, %lu:%s:%s\n", renderer_pcmtype.nChannels,
      ((float)renderer_pcmtype.nSamplingRate) / 1000,
      renderer_pcmtype.nBitPerSample,
      renderer_pcmtype.eNumData == OMX_NumericalDataSigned ? "s" : "u",
      renderer_pcmtype.eEndian == OMX_EndianBig ? "b" : "l");
}
