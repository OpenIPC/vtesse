#include "netcore/tunnel.h"

const char *tunnel_strerror(int err)
{
    switch (err) {
    case TUNNEL_OK:                         return "ok";
    case TUNNEL_ERR_CONFIG:                 return "invalid configuration";
    case TUNNEL_ERR_NOMEM:                  return "out of memory";
    case TUNNEL_ERR_RESOLVE:                return "cannot resolve server address";
    case TUNNEL_ERR_CONNECT:                return "cannot connect to server";
    case TUNNEL_ERR_TIMEOUT:                return "timeout";
    case TUNNEL_ERR_IO:                     return "connection error";
    case TUNNEL_ERR_PEER_CLOSED:            return "closed by server (possibly taken over by another connection)";
    case TUNNEL_ERR_KEEPALIVE:              return "no response from server (keepalive)";
    case TUNNEL_ERR_PROTOCOL:               return "protocol error (not a VTun server?)";
    case TUNNEL_ERR_AUTH:                   return "wrong session name or password";
    case TUNNEL_ERR_UNSUPPORTED_TYPE:       return "unsupported session type";
    case TUNNEL_ERR_UNSUPPORTED_TRANSPORT:  return "unsupported transport (UDP)";
    case TUNNEL_ERR_COMPRESSION:            return "compression is enabled on the server";
    case TUNNEL_ERR_ENCRYPTION:             return "encryption is enabled on the server";
    case TUNNEL_ERR_CLOSED:                 return "closed";
    default:                                return "unknown error";
    }
}

const char *tunnel_event_name(int event)
{
    switch (event) {
    case TUNNEL_EV_CONNECTING: return "connecting";
    case TUNNEL_EV_UP:         return "up";
    case TUNNEL_EV_DOWN:       return "down";
    case TUNNEL_EV_FAILED:     return "failed";
    default:                   return "?";
    }
}
