#define LWS_DLL
#define LWS_INTERNAL
#include <libwebsockets.h>
#include <stdint.h>

#include "limits.h"
#include "node.h"
#include "action.h"
#include "gambezi_generator.h"
#include "protocol_gambezi.h"
#include "subscription.h"

static void
uv_timeout_cb_update(uv_timer_t *w
#if UV_VERSION_MAJOR == 0
		, int status
#endif
)
{
	struct per_session_data_gambezi *pss = lws_container_of(
		w,
		struct per_session_data_gambezi,
		timer);

	// Handle subscriptions
	for(int i = 0;i < MAX_SUBSCRIPTIONS;i++)
	{
		struct Subscription* subscription = &(pss->subscriptions[i]);
		if(subscription->node)
		{
			subscription->count++;
			if(subscription->count >= subscription->period)
			{
				struct Action* action = addAction(pss->actions);
				action->type = DataReturnRequest;
				action->action.dataReturnRequest.node = subscription->node;
				subscription->count = 0;
			}
		}
	}

	// Write data
	lws_callback_on_writable(pss->wsi);
}

static int
callback_gambezi(struct lws *wsi,
                 enum lws_callback_reasons reason,
                 void *user,
                 void *in,
                 size_t len)
{
	// Websockets information
	struct per_session_data_gambezi *pss =
		(struct per_session_data_gambezi *) user;
	struct per_vhost_data_gambezi *vhd =
		(struct per_vhost_data_gambezi *) lws_protocol_vh_priv_get(lws_get_vhost(wsi),
		                                                           lws_get_protocol(wsi));

	////////////////////////////////////////////////////////////////////////////////
	// Why is the callback called
	switch (reason)
	{
		////////////////////////////////////////////////////////////////////////////////
		// Entire protocol setup
		case LWS_CALLBACK_PROTOCOL_INIT:
		{
			// Init websockets stuff
			vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
			                                  lws_get_protocol(wsi),
			                                  sizeof(struct per_vhost_data_gambezi));
			vhd->context = lws_get_context(wsi);

			// Init data holders
			uint8_t root_data[1];
			root_data[0] = 0;
			vhd->root_node = node_init(root_data, root_data, 0);
			vhd->root_node->key[0] = 0;
			break;
		}

		////////////////////////////////////////////////////////////////////////////////
		// Cleanup
		case LWS_CALLBACK_PROTOCOL_DESTROY:
		{
			if (!vhd)
				break;
			node_free_all(vhd->root_node);
			break;
		}

		////////////////////////////////////////////////////////////////////////////////
		// Initialize data related to a single connection
		case LWS_CALLBACK_ESTABLISHED:
		{
			// Session init
			pss->actions = malloc(sizeof(struct ActionQueue));
			initActionQueue(pss->actions);
			pss->wsi = wsi;
			uv_timer_init(lws_uv_getloop(vhd->context, 0),
			              &(pss->timer));
			uv_timer_start(&(pss->timer),
			               uv_timeout_cb_update, DEFAULT_PERIOD, DEFAULT_PERIOD);
			break;
		}

		////////////////////////////////////////////////////////////////////////////////
		// Connection destroyed
		case LWS_CALLBACK_CLOSED:
		{
			free(pss->actions);
			uv_timer_stop(&(pss->timer));
			break;
		}

		////////////////////////////////////////////////////////////////////////////////
		// Writing back to the client
		case LWS_CALLBACK_SERVER_WRITEABLE:
		{
			for(;;)
			{
				struct Action* action = useAction(pss->actions);

				// Bail if there is no action
				if(!action) {
					break;
				}

				// Process queued responses
				switch(action->type)
				{
					case PregeneratedRequest:
					{
						uint8_t* buffer = action->action.pregeneratedRequest.buffer + LWS_PRE;
						int length = action->action.pregeneratedRequest.length;
						lws_write(wsi, buffer, length, LWS_WRITE_BINARY);
						break;
					}
					case DataReturnRequest:
					{
						struct Node* node = action->action.dataReturnRequest.node;
						lws_write(wsi, node->buffer, node->current_length, LWS_WRITE_BINARY);
						break;
					}
				}
			}
			break;
		}

		////////////////////////////////////////////////////////////////////////////////
		// Data from client
		case LWS_CALLBACK_RECEIVE:
		{
			uint8_t* data = (uint8_t*)in;
			switch(data[0])
			{
				// Client requested key ID
				case 0:
				{
					// Get the ID
					uint8_t* parent_key;
					uint8_t* name;
					readIDRequestPacket(data, &parent_key, &name);
					struct Node* node = get_node_with_id(vhd->root_node,
					                                     parent_key,
					                                     name);

					// Queue and generate the response
					struct Action* action = addAction(pss->actions);
					action->type = PregeneratedRequest;
					uint8_t* buffer = action->action.pregeneratedRequest.buffer + LWS_PRE;
					int length = writeIDResponsePacket(buffer, BUFFER_LENGTH, node);
					action->action.pregeneratedRequest.length = length;

					// Request callback to write to client
					lws_callback_on_writable(wsi);

					break;
				}
				// Client set key value
				case 1:
				{
					// Read packet
					uint8_t* key;
					uint16_t length;
					uint8_t* value;
					readValueSetPacket(data, &key, &length, &value);
					struct Node* node = node_traverse(vhd->root_node, key);

					// Set value and update subscribers
					node_set_value(node, value, length);
					node_notify_subscribers(node);
					break;
				}
				// Client set refresh rate
				case 2:
				{
					uint16_t refresh_rate;
					readRefreshRateSetPacket(data, &refresh_rate);
					uv_timer_set_repeat(&(pss->timer), refresh_rate);
					break;
				}
				// Client update subscription
				case 3:
				{
					// Read packet
					uint8_t* key;
					uint8_t set_children;
					uint16_t refresh_skip;
					readSubscriptionUpdatePacket(data, &key, &set_children, &refresh_skip);
					struct Node* node = node_traverse(vhd->root_node, key);

					// Get updates as fast as possible
					if(refresh_skip == 0x0000)
					{
						node_add_subscriber(node, pss);
					}
					// Unsubscribe
					else if(refresh_skip == 0xFFFF)
					{
						node_remove_subscriber(node, pss);
						struct Subscription* subscription = subscription_get_with_node(
							pss->subscriptions,
							node);
						if(subscription)
						{
							subscription->node = 0;
						}
					}
					// Update at fixed rate
					else
					{
						struct Subscription* subscription = subscription_get_with_node(
							pss->subscriptions,
							node);
						// Update existing subscription
						if(subscription)
						{
							if(subscription->period != refresh_skip)
							{
								subscription->period = refresh_skip;
								subscription->count = 0;
							}
						}
						// Create new subscription
						else
						{
							subscription = subscription_get_empty(pss->subscriptions);
							// Bail if there is no more room
							if(!subscription)
							{
								lwsl_err("Too many subscriptions");
							}
							subscription->node = node;
							subscription->period = refresh_skip;
							subscription->count = 0;
						}
					}
					break;
				}
				// Client request node value
				case 4:
				{
					uint8_t* key;
					uint8_t get_children;
					readValueRequestPacket(data, &key, &get_children);
					struct Node* node = node_traverse(vhd->root_node, key);

					// Queue and generate the response
					struct Action* action = addAction(pss->actions);
					action->type = DataReturnRequest;
					action->action.dataReturnRequest.node = node;

					// Request callback to write to client
					lws_callback_on_writable(wsi);

					break;
				}
			}
			break;
		}
	}

	return 0;
}

static const struct lws_protocols protocols[] =
{
	{
		"gambezi-protocol",
		callback_gambezi,
		sizeof(struct per_session_data_gambezi),
		BUFFER_LENGTH, /* rx buf size must be >= permessage-deflate rx size */
	},
};

LWS_VISIBLE int
init_protocol_gambezi(struct lws_context *context,
                      struct lws_plugin_capability *c)
{
	if (c->api_magic != LWS_PLUGIN_API_MAGIC)
	{
		lwsl_err("Plugin API %d, library API %d",
		         LWS_PLUGIN_API_MAGIC,
		         c->api_magic);
		return 1;
	}

	c->protocols = protocols;
	c->count_protocols = ARRAY_SIZE(protocols);
	c->extensions = NULL;
	c->count_extensions = 0;

	return 0;
}

LWS_VISIBLE int
destroy_protocol_gambezi(struct lws_context *context)
{
	return 0;
}
