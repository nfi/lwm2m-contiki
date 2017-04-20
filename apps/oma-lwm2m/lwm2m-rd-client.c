/*
 * Copyright (c) 2015, Yanzi Networks AB.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \addtogroup oma-lwm2m
 * @{
 */

/**
 * \file
 *         Implementation of the Contiki OMA LWM2M engine
 *         Registration and bootstrap client
 * \author
 *         Joakim Eriksson <joakime@sics.se>
 *         Niclas Finne <nfi@sics.se>
 *         Joel Hoglund <joel@sics.se>
 */

#include "lwm2m-engine.h"
#include "lwm2m-object.h"
#include "lwm2m-device.h"
#include "lwm2m-plain-text.h"
#include "lwm2m-json.h"
#include "lwm2m-rd-client.h"
#include "er-coap.h"
#include "er-coap-engine.h"
#include "er-coap-endpoint.h"
#include "er-coap-callback-api.h"
#include "oma-tlv.h"
#include "oma-tlv-reader.h"
#include "oma-tlv-writer.h"
#include "lwm2m-security.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#if UIP_CONF_IPV6_RPL
#include "net/rpl/rpl.h"
#endif /* UIP_CONF_IPV6_RPL */

#define DEBUG 0
#if DEBUG
#include <stdio.h>
#define PRINTF(...) printf(__VA_ARGS__)
#define PRINTS(l,s,f) do { int i;					\
    for(i = 0; i < l; i++) printf(f, s[i]); \
    } while(0)
#define PRINT6ADDR(addr) PRINTF("[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]", ((uint8_t *)addr)[0], ((uint8_t *)addr)[1], ((uint8_t *)addr)[2], ((uint8_t *)addr)[3], ((uint8_t *)addr)[4], ((uint8_t *)addr)[5], ((uint8_t *)addr)[6], ((uint8_t *)addr)[7], ((uint8_t *)addr)[8], ((uint8_t *)addr)[9], ((uint8_t *)addr)[10], ((uint8_t *)addr)[11], ((uint8_t *)addr)[12], ((uint8_t *)addr)[13], ((uint8_t *)addr)[14], ((uint8_t *)addr)[15])
#define PRINTLLADDR(lladdr) PRINTF("[%02x:%02x:%02x:%02x:%02x:%02x]", (lladdr)->addr[0], (lladdr)->addr[1], (lladdr)->addr[2], (lladdr)->addr[3], (lladdr)->addr[4], (lladdr)->addr[5])
#else
#define PRINTF(...)
#define PRINTS(l,s,f)
#define PRINT6ADDR(addr)
#define PRINTLLADDR(addr)
#endif

#ifndef LWM2M_DEFAULT_CLIENT_LIFETIME
#define LWM2M_DEFAULT_CLIENT_LIFETIME 10 /* sec */
#endif

#define REMOTE_PORT        UIP_HTONS(COAP_DEFAULT_PORT)
#define BS_REMOTE_PORT     UIP_HTONS(5685)

#define STATE_MACHINE_UPDATE_INTERVAL 500

static struct lwm2m_session_info session_info;
static struct request_state rd_request_state;

/* The states for the RD client state machine */
/* When node is unregistered it ends up in UNREGISTERED
   and this is going to be there until use X or Y kicks it
   back into INIT again */
#define INIT               0
#define WAIT_NETWORK       1
#define DO_BOOTSTRAP       3
#define BOOTSTRAP_SENT     4
#define BOOTSTRAP_DONE     5
#define DO_REGISTRATION    6
#define REGISTRATION_SENT  7
#define REGISTRATION_DONE  8
#define UPDATE_SENT        9
#define DEREGISTER        10
#define DEREGISTER_SENT   11
#define DEREGISTERED      12
#define DISCONNECTED      13

#define FLAG_RD_DATA_DIRTY 1
#define FLAG_RD_DATA_UPDATE_ON_DIRTY 0x10

static uint8_t rd_state = 0;
static uint8_t rd_flags = FLAG_RD_DATA_UPDATE_ON_DIRTY;
static uint64_t wait_until_network_check = 0;
static uint64_t last_update;

static char path_data[32]; /* allocate some data for building the path */
static char query_data[64]; /* allocate some data for queries and updates */
static uint8_t rd_data[128]; /* allocate some data for the RD */

static ntimer_t rd_timer;

void check_periodic_observations();

/*---------------------------------------------------------------------------*/
static void
prepare_update(coap_packet_t *request) {
  int len;
  coap_init_message(request, COAP_TYPE_CON, COAP_POST, 0);
  coap_set_header_uri_path(request, session_info.assigned_ep);

  snprintf(query_data, sizeof(query_data) - 1, "?lt=%d", session_info.lifetime);
  printf("UPDATE:%s %s\n", session_info.assigned_ep, query_data);
  coap_set_header_uri_query(request, query_data);

  if(rd_flags & FLAG_RD_DATA_DIRTY) {
    rd_flags &= ~FLAG_RD_DATA_DIRTY;
    /* generate the rd data */
    len = lwm2m_engine_get_rd_data(rd_data, sizeof(rd_data));
    coap_set_payload(request, rd_data, len);
  }
}

static int
has_network_access(void)
{
#if UIP_CONF_IPV6_RPL
  if(rpl_get_any_dag() == NULL) {
    return 0;
  }
#endif /* UIP_CONF_IPV6_RPL */
  return 1;
}
/*---------------------------------------------------------------------------*/
int
lwm2m_rd_client_is_registered(void)
{
  return rd_state == REGISTRATION_DONE || rd_state == UPDATE_SENT;
}
/*---------------------------------------------------------------------------*/
void
lwm2m_rd_client_use_bootstrap_server(int use)
{
  session_info.use_bootstrap = use != 0;
  if(session_info.use_bootstrap) {
    rd_state = INIT;
  }
}
/*---------------------------------------------------------------------------*/
/* will take another argument when we support multiple sessions */
void
lwm2m_rd_client_set_session_callback(session_callback_t cb)
{
  session_info.callback = cb;
}
/*---------------------------------------------------------------------------*/
static void
perform_session_callback(int state)
{
  if(session_info.callback != NULL) {
    PRINTF("Performing session callback: %d cb:%p\n",
           state, session_info.callback);
    session_info.callback(&session_info, state);
  }
}
/*---------------------------------------------------------------------------*/
void
lwm2m_rd_client_use_registration_server(int use)
{
  session_info.use_registration = use != 0;
  if(session_info.use_registration) {
    rd_state = INIT;
  }
}
/*---------------------------------------------------------------------------*/
uint16_t
lwm2m_rd_client_get_lifetime(void)
{
  return session_info.lifetime;
}
/*---------------------------------------------------------------------------*/
void
lwm2m_rd_client_set_lifetime(uint16_t lifetime)
{
  if(lifetime > 0) {
    session_info.lifetime = lifetime;
  } else {
    session_info.lifetime = LWM2M_DEFAULT_CLIENT_LIFETIME;
  }
}
/*---------------------------------------------------------------------------*/
void
lwm2m_rd_client_set_update_rd(void)
{
  rd_flags |= FLAG_RD_DATA_DIRTY;
}
/*---------------------------------------------------------------------------*/
void
lwm2m_rd_client_set_automatic_update(int update)
{
  rd_flags = (rd_flags & ~FLAG_RD_DATA_UPDATE_ON_DIRTY) |
    (update != 0 ? FLAG_RD_DATA_UPDATE_ON_DIRTY : 0);
}
/*---------------------------------------------------------------------------*/
void
lwm2m_rd_client_register_with_server(const coap_endpoint_t *server)
{
  coap_endpoint_copy(&session_info.server_ep, server);
  session_info.has_registration_server_info = 1;
  session_info.registered = 0;
  if(session_info.use_registration) {
    rd_state = INIT;
  }
}
/*---------------------------------------------------------------------------*/
static int
update_registration_server(void)
{
  if(session_info.has_registration_server_info) {
    return 1;
  }

#if UIP_CONF_IPV6_RPL
  {
    rpl_dag_t *dag;

    /* Use the DAG id as server address if no other has been specified */
    dag = rpl_get_any_dag();
    if(dag != NULL) {
      /* create coap-endpoint? */
      /* uip_ipaddr_copy(&server_ipaddr, &dag->dag_id); */
      /* server_port = REMOTE_PORT; */
      return 1;
    }
  }
#endif /* UIP_CONF_IPV6_RPL */

  return 0;
}
/*---------------------------------------------------------------------------*/
void
lwm2m_rd_client_register_with_bootstrap_server(const coap_endpoint_t *server)
{
  coap_endpoint_copy(&session_info.bs_server_ep, server);
  session_info.has_bs_server_info = 1;
  session_info.bootstrapped = 0;
  session_info.registered = 0;
  if(session_info.use_bootstrap) {
    rd_state = INIT;
  }
}
/*---------------------------------------------------------------------------*/
void
lwm2m_rd_client_deregister(void)
{
  if(lwm2m_rd_client_is_registered()) {
    rd_state = DEREGISTER;
  }
}
/*---------------------------------------------------------------------------*/
static int
update_bootstrap_server(void)
{
  if(session_info.has_bs_server_info) {
    return 1;
  }

#if UIP_CONF_IPV6_RPL
  {
    rpl_dag_t *dag;

    /* Use the DAG id as server address if no other has been specified */
    dag = rpl_get_any_dag();
    if(dag != NULL) {
      /* create coap endpoint */
      /* uip_ipaddr_copy(&bs_server_ipaddr, &dag->dag_id); */
      /* bs_server_port = REMOTE_PORT; */
      return 1;
    }
  }
#endif /* UIP_CONF_IPV6_RPL */

  return 0;
}
/*---------------------------------------------------------------------------*/
/*
 * A client initiated bootstrap starts with a POST to /bs?ep={session_info.ep},
 * on the bootstrap server. The server should reply with 2.04.
 * The server will thereafter do DELETE and or PUT to write new client objects.
 * The bootstrap finishes with the server doing POST to /bs on the client.
 *
 * Page 64 in 07 April 2016 spec.
 *
 * TODO
 */
static void
bootstrap_callback(struct request_state *state)
{
  PRINTF("Bootstrap callback Response: %d, ", state->response != NULL);
  if(state->response) {
    if(CHANGED_2_04 == state->response->code) {
      PRINTF("Considered done!\n");
      rd_state = BOOTSTRAP_DONE;
      return;
    }
    /* Possible error response codes are 4.00 Bad request & 4.15 Unsupported content format */
    PRINTF("Failed with code %d. Retrying\n", state->response->code);
    /* TODO Application callback? */
    rd_state = INIT;
  } else if(BOOTSTRAP_SENT == rd_state) { /* this can handle double invocations */
    /* Failure! */
    PRINTF("Bootstrap failed! Retry?");
    rd_state = DO_BOOTSTRAP;
  } else {
    PRINTF("Ignore\n");
  }
}
/*---------------------------------------------------------------------------*/
/*
 * Page 65-66 in 07 April 2016 spec.
 */
static void
registration_callback(struct request_state *state)
{
  PRINTF("Registration callback. Response: %d, ", state->response != NULL);
  if(state->response) {
    /* check state and possibly set registration to done */
    if(CREATED_2_01 == state->response->code) {
      if(state->response->location_path_len < LWM2M_RD_CLIENT_ASSIGNED_ENDPOINT_MAX_LEN) {
        memcpy(session_info.assigned_ep, state->response->location_path,
               state->response->location_path_len);
        session_info.assigned_ep[state->response->location_path_len] = 0;
        /* if we decide to not pass the lt-argument on registration, we should force an initial "update" to register lifetime with server */
        rd_state = REGISTRATION_DONE;
        /* remember the last reg time */
        last_update = ntimer_uptime();
        PRINTF("Done (assigned EP='%s')!\n", session_info.assigned_ep);
        perform_session_callback(LWM2M_RD_CLIENT_REGISTERED);
        return;
      }

      PRINTF("failed to handle assigned EP: '");
      PRINTS(state->response->location_path_len,
             state->response->location_path, "%c");
      PRINTF("'. Re-init network.\n");
    } else {
      /* Possible error response codes are 4.00 Bad request & 4.03 Forbidden */
      PRINTF("failed with code %d. Re-init network\n", state->response->code);
    }
    /* TODO Application callback? */
    rd_state = INIT;
  } else if(REGISTRATION_SENT == rd_state) { /* this can handle double invocations */
    /* Failure! */
    PRINTF("Registration failed! Retry?\n");
    rd_state = DO_REGISTRATION;
  } else {
    PRINTF("Ignore\n");
  }
}
/*---------------------------------------------------------------------------*/
/*
 * Page 65-66 in 07 April 2016 spec.
 */
static void
update_callback(struct request_state *state)
{
  PRINTF("Update callback. Response: %d, ", state->response != NULL);

  if(state->response) {
    if(CHANGED_2_04 == state->response->code) {
      PRINTF("Done!\n");
      /* remember the last reg time */
      last_update = ntimer_uptime();
      rd_state = REGISTRATION_DONE;
      return;
    }
    /* Possible error response codes are 4.00 Bad request & 4.04 Not Found */
    PRINTF("Failed with code %d. Retrying registration\n", state->response->code);
    rd_state = DO_REGISTRATION;
  } else if(REGISTRATION_SENT == rd_state) { /* this can handle the current double invocation */
    /*Failure! */
    PRINTF("Registration failed! Retry?");
    rd_state = DO_REGISTRATION;
  } else if(UPDATE_SENT == rd_state) {
    /* Update failed */
    PRINTF("Update failed! Retry?");
    rd_state = DO_REGISTRATION;
  } else {
    PRINTF("Ignore\n");
  }
}
/*---------------------------------------------------------------------------*/
static void
deregister_callback(struct request_state *state)
{
  PRINTF("Deregister callback. Response Code: %d\n",
         state->response != NULL ? state->response->code : 0);

  if(state->response) {
    if(DELETED_2_02 == state->response->code) {
      PRINTF("Deregistration success\n");
      rd_state = DEREGISTERED;
      perform_session_callback(LWM2M_RD_CLIENT_DEREGISTERED);
    }
  } else {
    /* failed? try again? */
    PRINTF("Deregistration failed\n");
    if(rd_state == DEREGISTER_SENT) {
      rd_state = DISCONNECTED;
      perform_session_callback(LWM2M_RD_CLIENT_DEREGISTER_TIMEOUT);
    }
  }
}
/*---------------------------------------------------------------------------*/
/* ntimer callback */
static void
periodic_process(ntimer_t *timer)
{
  static coap_packet_t request[1];      /* This way the packet can be treated as pointer as usual. */
  uint64_t now;

  /* reschedule the ntimer */
  ntimer_reset(&rd_timer, STATE_MACHINE_UPDATE_INTERVAL);
  now = ntimer_uptime();

  PRINTF("RD Client - state: %d, ms: %lu\n", rd_state,
         (unsigned long) ntimer_uptime());

  switch(rd_state) {
  case INIT:
    PRINTF("RD Client started with endpoint '%s' and client lifetime %d\n", session_info.ep, session_info.lifetime);
    if(coap_endpoint_connect(&session_info.server_ep)) {
      rd_state = WAIT_NETWORK;
    } else {
      PRINTF("Failed to connect... trying again...\n");
    }
    break;
  case WAIT_NETWORK:
    if(now > wait_until_network_check) {
      /* check each 10 seconds before next check */
      PRINTF("Checking for network... %lu\n",
             (unsigned long)wait_until_network_check);
      wait_until_network_check = now + 10000;
      if(has_network_access()) {
        /* Either do bootstrap then registration */
        if(session_info.use_bootstrap) {
          rd_state = DO_BOOTSTRAP;
        } else {
          rd_state = DO_REGISTRATION;
        }
      }
      /* Otherwise wait until for a network to join */
    }
    break;
  case DO_BOOTSTRAP:
    if(session_info.use_bootstrap && session_info.bootstrapped == 0) {
      if(update_bootstrap_server()) {
        /* prepare request, TID is set by COAP_BLOCKING_REQUEST() */
        coap_init_message(request, COAP_TYPE_CON, COAP_POST, 0);
        coap_set_header_uri_path(request, "/bs");

        snprintf(query_data, sizeof(query_data) - 1, "?ep=%s", session_info.ep);
        coap_set_header_uri_query(request, query_data);
        PRINTF("Registering ID with bootstrap server [");
        coap_endpoint_print(&session_info.bs_server_ep);
        PRINTF("] as '%s'\n", query_data);

        coap_send_request(&rd_request_state, &session_info.bs_server_ep,
                          request, bootstrap_callback);

        rd_state = BOOTSTRAP_SENT;
      }
    }
    break;
  case BOOTSTRAP_SENT:
    /* Just wait for bootstrap to be done...  */
    break;
  case BOOTSTRAP_DONE:
    /* check that we should still use bootstrap */
    if(session_info.use_bootstrap) {
      const lwm2m_security_value_t *security = NULL;
      int i;
      PRINTF("*** Bootstrap - checking for server info...\n");
      /* get the security object - ignore bootstrap servers */
      for(i = 0; i < lwm2m_security_instance_count(); i++) {
        security = lwm2m_security_get_instance(i);
        if(security != NULL && security->bootstrap == 0)
          break;
        security = NULL;
      }

      if(security != NULL) {
        /* get the server URI */
        if(security->server_uri_len > 0) {
          uint8_t secure = 0;

          PRINTF("**** Found security instance using: ");
          PRINTS(security->server_uri_len, security->server_uri, "%c");
          PRINTF(" (len %d) \n", security->server_uri_len);
          /* TODO Should verify it is a URI */
          /* Check if secure */
          secure = strncmp((const char *)security->server_uri,
                           "coaps:", 6) == 0;

          if(!coap_endpoint_parse((const char *)security->server_uri,
                                  security->server_uri_len,
                                  &session_info.server_ep)) {
            PRINTF("Failed to parse server URI!\n");
          } else {
            PRINTF("Server address:");
            coap_endpoint_print(&session_info.server_ep);
            PRINTF("\n");
            if(secure) {
              PRINTF("Secure CoAP requested but not supported - can not bootstrap\n");
            } else {
              lwm2m_rd_client_register_with_server(&session_info.server_ep);
              session_info.bootstrapped++;
            }
          }
        } else {
          PRINTF("** failed to parse URI ");
          PRINTS(security->server_uri_len, security->server_uri, "%c");
          PRINTF("\n");
        }
      }

      /* if we did not register above - then fail this and restart... */
      if(session_info.bootstrapped == 0) {
        /* Not ready. Lets retry with the bootstrap server again */
        rd_state = DO_BOOTSTRAP;
      } else {
        rd_state = DO_REGISTRATION;
      }
    }
    break;
  case DO_REGISTRATION:
    if(!coap_endpoint_is_connected(&session_info.server_ep)) {
      /* Not connected... wait a bit... */
      printf("Wait until connected... \n");
      return;
    }
    if(session_info.use_registration && !session_info.registered &&
       update_registration_server()) {

      int len;

      /* prepare request, TID was set by COAP_BLOCKING_REQUEST() */
      coap_init_message(request, COAP_TYPE_CON, COAP_POST, 0);
      coap_set_header_uri_path(request, "/rd");

      snprintf(query_data, sizeof(query_data) - 1, "?ep=%s&lt=%d", session_info.ep, session_info.lifetime);
      coap_set_header_uri_query(request, query_data);

      /* generate the rd data */
      len = lwm2m_engine_get_rd_data(rd_data, sizeof(rd_data));
      coap_set_payload(request, rd_data, len);

      PRINTF("Registering with [");
      coap_endpoint_print(&session_info.server_ep);
      PRINTF("] lwm2m endpoint '%s': '", query_data);
      PRINTS(len, rd_data, "%c");
      PRINTF("'\n");
      coap_send_request(&rd_request_state, &session_info.server_ep,
                        request, registration_callback);
      rd_state = REGISTRATION_SENT;
    }
  case REGISTRATION_SENT:
    /* just wait until the callback kicks us to the next state... */
    break;
  case REGISTRATION_DONE:
    /* All is done! */

    check_periodic_observations(); /* TODO: manage periodic observations */

    /* check if it is time for the next update */
    if(((uint32_t)session_info.lifetime * 500) <= now - last_update) { /* time to send an update to the server, at half-time! sec vs ms */
      /* prepare request,  */
      prepare_update(request);
      coap_send_request(&rd_request_state, &session_info.server_ep, request,
                        update_callback);

      rd_state = UPDATE_SENT;
    }
    break;

  case UPDATE_SENT:
    /* just wait until the callback kicks us to the next state... */
    break;
  case DEREGISTER:
    PRINTF("DEREGISTER %s\n", session_info.assigned_ep);
    coap_init_message(request, COAP_TYPE_CON, COAP_DELETE, 0);
    coap_set_header_uri_path(request, session_info.assigned_ep);
    coap_send_request(&rd_request_state, &session_info.server_ep, request,
                      deregister_callback);
    rd_state = DEREGISTER_SENT;
    break;
  case DEREGISTER_SENT:
    break;
  case DEREGISTERED:
    break;
  case DISCONNECTED:
    break;

  default:
    PRINTF("Unhandled state: %d\n", rd_state);
  }
}
/*---------------------------------------------------------------------------*/
void
lwm2m_rd_client_init(const char *ep)
{
  session_info.ep = ep;
  if(session_info.lifetime == 0) {
    session_info.lifetime = LWM2M_DEFAULT_CLIENT_LIFETIME;
  }
  rd_state = INIT;
  /* Example using network timer */
  ntimer_set_callback(&rd_timer, periodic_process);
  ntimer_set(&rd_timer, STATE_MACHINE_UPDATE_INTERVAL); /* call the RD client 2 times per second */
}
/*---------------------------------------------------------------------------*/
void
check_periodic_observations(void)
{
/* TODO */
}
/*---------------------------------------------------------------------------*/
