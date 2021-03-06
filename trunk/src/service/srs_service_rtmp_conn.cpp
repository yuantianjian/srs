/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2017 OSSRS(winlin)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <srs_service_rtmp_conn.hpp>

#include <unistd.h>
using namespace std;

#include <srs_protocol_kbps.hpp>
#include <srs_rtmp_stack.hpp>
#include <srs_service_st.hpp>
#include <srs_protocol_amf0.hpp>
#include <srs_protocol_utility.hpp>
#include <srs_service_utility.hpp>

SrsBasicRtmpClient::SrsBasicRtmpClient(string u, int64_t ctm, int64_t stm)
{
    kbps = new SrsKbps();
    
    url = u;
    connect_timeout = ctm;
    stream_timeout = stm;
    
    req = new SrsRequest();
    srs_parse_rtmp_url(url, req->tcUrl, req->stream);
    srs_discovery_tc_url(req->tcUrl, req->schema, req->host, req->vhost, req->app, req->port, req->param);
    
    transport = NULL;
    client = NULL;
    
    stream_id = 0;
}

SrsBasicRtmpClient::~SrsBasicRtmpClient()
{
    close();
    srs_freep(kbps);
}

srs_error_t SrsBasicRtmpClient::connect()
{
    srs_error_t err = srs_success;
    
    close();
    
    transport = new SrsTcpClient(req->host, req->port, connect_timeout);
    client = new SrsRtmpClient(transport);
    kbps->set_io(transport, transport);
    
    if ((err = transport->connect()) != srs_success) {
        close();
        return srs_error_wrap(err, "transport connect");
    }
    
    client->set_recv_timeout(stream_timeout);
    client->set_send_timeout(stream_timeout);
    
    // connect to vhost/app
    if ((err = client->handshake()) != srs_success) {
        return srs_error_wrap(err, "handshake");
    }
    if ((err = connect_app()) != srs_success) {
        return srs_error_wrap(err, "connect tcUrl");
    }
    if ((err = client->create_stream(stream_id)) != srs_success) {
        return srs_error_wrap(err, "create stream");
    }
    
    return err;
}

void SrsBasicRtmpClient::close()
{
    kbps->set_io(NULL, NULL);
    srs_freep(client);
    srs_freep(transport);
}

srs_error_t SrsBasicRtmpClient::connect_app()
{
    return do_connect_app(srs_get_public_internet_address(), false);
}

srs_error_t SrsBasicRtmpClient::do_connect_app(string local_ip, bool debug)
{
    srs_error_t err = srs_success;
    
    // args of request takes the srs info.
    if (req->args == NULL) {
        req->args = SrsAmf0Any::object();
    }
    
    // notify server the edge identity,
    // @see https://github.com/ossrs/srs/issues/147
    SrsAmf0Object* data = req->args;
    data->set("srs_sig", SrsAmf0Any::str(RTMP_SIG_SRS_KEY));
    data->set("srs_server", SrsAmf0Any::str(RTMP_SIG_SRS_SERVER));
    data->set("srs_license", SrsAmf0Any::str(RTMP_SIG_SRS_LICENSE));
    data->set("srs_role", SrsAmf0Any::str(RTMP_SIG_SRS_ROLE));
    data->set("srs_url", SrsAmf0Any::str(RTMP_SIG_SRS_URL));
    data->set("srs_version", SrsAmf0Any::str(RTMP_SIG_SRS_VERSION));
    data->set("srs_site", SrsAmf0Any::str(RTMP_SIG_SRS_WEB));
    data->set("srs_email", SrsAmf0Any::str(RTMP_SIG_SRS_EMAIL));
    data->set("srs_copyright", SrsAmf0Any::str(RTMP_SIG_SRS_COPYRIGHT));
    data->set("srs_primary", SrsAmf0Any::str(RTMP_SIG_SRS_PRIMARY));
    data->set("srs_authors", SrsAmf0Any::str(RTMP_SIG_SRS_AUTHROS));
    // for edge to directly get the id of client.
    data->set("srs_pid", SrsAmf0Any::number(getpid()));
    data->set("srs_id", SrsAmf0Any::number(_srs_context->get_id()));
    
    // local ip of edge
    data->set("srs_server_ip", SrsAmf0Any::str(local_ip.c_str()));
    
    // generate the tcUrl
    std::string param = "";
    std::string target_vhost = req->vhost;
    std::string tc_url = srs_generate_tc_url(req->host, req->vhost, req->app, req->port, param);
    
    // replace the tcUrl in request,
    // which will replace the tc_url in client.connect_app().
    req->tcUrl = tc_url;
    
    // upnode server identity will show in the connect_app of client.
    // @see https://github.com/ossrs/srs/issues/160
    // the debug_srs_upnode is config in vhost and default to true.
    if ((err = client->connect_app(req->app, tc_url, req, debug, NULL)) != srs_success) {
        return srs_error_wrap(err, "connect tcUrl=%s, debug=%d", tc_url.c_str(), debug);
    }
    
    return err;
}

srs_error_t SrsBasicRtmpClient::publish()
{
    srs_error_t err = srs_success;
    
    // publish.
    if ((err = client->publish(req->stream, stream_id)) != srs_success) {
        return srs_error_wrap(err, "publish stream=%s", req->stream.c_str());
    }
    
    return err;
}

srs_error_t SrsBasicRtmpClient::play()
{
    srs_error_t err = srs_success;
    
    if ((err = client->play(req->stream, stream_id)) != srs_success) {
        return srs_error_wrap(err, "play stream=%s", req->stream.c_str());
    }
    
    return err;
}

void SrsBasicRtmpClient::kbps_sample(const char* label, int64_t age)
{
    kbps->sample();
    
    int sr = kbps->get_send_kbps();
    int sr30s = kbps->get_send_kbps_30s();
    int sr5m = kbps->get_send_kbps_5m();
    int rr = kbps->get_recv_kbps();
    int rr30s = kbps->get_recv_kbps_30s();
    int rr5m = kbps->get_recv_kbps_5m();
    
    srs_trace("<- %s time=%" PRId64 ", okbps=%d,%d,%d, ikbps=%d,%d,%d", label, age, sr, sr30s, sr5m, rr, rr30s, rr5m);
}

void SrsBasicRtmpClient::kbps_sample(const char* label, int64_t age, int msgs)
{
    kbps->sample();
    
    int sr = kbps->get_send_kbps();
    int sr30s = kbps->get_send_kbps_30s();
    int sr5m = kbps->get_send_kbps_5m();
    int rr = kbps->get_recv_kbps();
    int rr30s = kbps->get_recv_kbps_30s();
    int rr5m = kbps->get_recv_kbps_5m();
    
    srs_trace("<- %s time=%" PRId64 ", msgs=%d, okbps=%d,%d,%d, ikbps=%d,%d,%d", label, age, msgs, sr, sr30s, sr5m, rr, rr30s, rr5m);
}

int SrsBasicRtmpClient::sid()
{
    return stream_id;
}

srs_error_t SrsBasicRtmpClient::recv_message(SrsCommonMessage** pmsg)
{
    return client->recv_message(pmsg);
}

srs_error_t SrsBasicRtmpClient::decode_message(SrsCommonMessage* msg, SrsPacket** ppacket)
{
    return client->decode_message(msg, ppacket);
}

srs_error_t SrsBasicRtmpClient::send_and_free_messages(SrsSharedPtrMessage** msgs, int nb_msgs)
{
    return client->send_and_free_messages(msgs, nb_msgs, stream_id);
}

srs_error_t SrsBasicRtmpClient::send_and_free_message(SrsSharedPtrMessage* msg)
{
    return client->send_and_free_message(msg, stream_id);
}

void SrsBasicRtmpClient::set_recv_timeout(int64_t timeout)
{
    transport->set_recv_timeout(timeout);
}

