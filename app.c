/* Required outside the BUILD_APPLICATIONS #ifdef! */
#include "ast_config.h"

#ifdef BUILD_APPLICATIONS
/*
   Copyright (C) 2009 - 2010

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>

   bg <bg_one@mail.ru>
*/

#include <asterisk/app.h>	/* AST_DECLARE_APP_ARGS() ... */
#include <asterisk/pbx.h>	/* pbx_builtin_setvar_helper() */
#include <asterisk/module.h>	/* ast_register_application2() ast_unregister_application() */

#include "ast_compat.h"		/* asterisk compatibility fixes */

#include "app.h"		/* app_register() app_unregister() */
#include "chan_quectel.h"	/* struct pvt */
#include "helpers.h"		/* send_sms() ITEMS_OF() */
#include "error.h"

struct ast_channel;

static int app_status_exec (struct ast_channel* channel, const char* data)
{
	struct pvt * pvt;
	char * parse;
	int stat;
	char status[2];
	int exists = 0;

	AST_DECLARE_APP_ARGS (args,
		AST_APP_ARG (resource);
		AST_APP_ARG (variable);
	);

	if (ast_strlen_zero (data))
	{
		return -1;
	}

	parse = ast_strdupa (data);

	AST_STANDARD_APP_ARGS (args, parse);

	if (ast_strlen_zero (args.resource) || ast_strlen_zero (args.variable))
	{
		return -1;
	}

	/* TODO: including options number */
	pvt = find_device_by_resource(args.resource, 0, NULL, &exists);
	if(pvt)
	{
		/* ready for outgoing call */
		ast_mutex_unlock (&pvt->lock);
		stat = 2;
	}
	else
	{
		stat = exists ? 3 : 1;
	}

	snprintf (status, sizeof (status), "%d", stat);
	pbx_builtin_setvar_helper (channel, args.variable, status);

	return 0;
}

static int app_send_sms_exec (attribute_unused struct ast_channel* channel, const char* data)
{
	char*	parse;

	AST_DECLARE_APP_ARGS (args,
		AST_APP_ARG (device);
		AST_APP_ARG (number);
		AST_APP_ARG (message);
		AST_APP_ARG (validity);
		AST_APP_ARG (report);
		AST_APP_ARG (payload);
	);

	if (ast_strlen_zero (data))
	{
		return -1;
	}

	parse = ast_strdupa (data);

	AST_STANDARD_APP_ARGS (args, parse);

	if (ast_strlen_zero (args.device))
	{
		ast_log (LOG_ERROR, "NULL device for message -- SMS will not be sent\n");
		return -1;
	}

	if (ast_strlen_zero (args.number))
	{
		ast_log (LOG_ERROR, "NULL destination for message -- SMS will not be sent\n");
		return -1;
	}

	if (ast_strlen_zero(args.payload))
	{
		ast_log(LOG_ERROR, "NULL payload for message -- SMS will not be sent\n");
		return -1;
	}

	if (send_sms(args.device, args.number, args.message, args.validity, args.report, args.payload, strlen(args.payload) + 1) < 0) {
		ast_log(LOG_ERROR, "[%s] %s\n", args.device, error2str(chan_quectel_err));
		return -1;
	}
	return 0;
}

static int app_send_ussd_exec(attribute_unused struct ast_channel* channel, const char* data)
{
	char* parse;

	AST_DECLARE_APP_ARGS(args,
		 AST_APP_ARG(device);
		 AST_APP_ARG(ussd);
	);

	if (ast_strlen_zero(data))
	{
		return -1;
	}

	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);

	if (ast_strlen_zero(args.device))
	{
		ast_log(LOG_ERROR, "NULL device for ussd -- USSD will not be sent\n");
		return -1;
	}

	if (ast_strlen_zero(args.ussd))
	{
		ast_log(LOG_ERROR, "NULL ussd command -- USSD will not be sent\n");
		return -1;
	}

	if (send_ussd(args.device, args.ussd) < 0) {
		ast_log(LOG_ERROR, "[%s] %s\n", args.device, error2str(chan_quectel_err));
		return -1;
	}
	return 0;
}


static const struct quectel_application
{
	const char*	name;

	int		(*func)(struct ast_channel* channel, const char* data);
	const char*	synopsis;
	const char*	desc;
} dca[] =
{
	{
		"QuectelStatus",
		app_status_exec,
		"QuectelStatus(Resource,Variable)",
		"QuectelStatus(Resource,Variable)\n"
		"  Resource - Resource string as for Dial()\n"
		"  Variable - Variable to store status in will be 1-3.\n"
		"             In order, Disconnected, Connected & Free, Connected & Busy.\n"
	},
	{
		"QuectelSendSMS",
		app_send_sms_exec,
		"QuectelSendSMS(Device,Dest,Message,Validity,Report,Payload)",
		"QuectelSendSMS(Device,Dest,Message,Validity,Report,Payload)\n"
		"  Device   - Id of device from quectel5g.conf\n"
		"  Dest     - destination\n"
		"  Message  - text of the message\n"
		"  Validity - Validity period in minutes\n"
		"  Report   - Boolean flag for report request\n"
		"  Payload  - Unstructured data that will be included in delivery report\n"
	},
	{
		"QuectelSendUSSD",
		app_send_ussd_exec,
		"QuectelSendUSSD(Device,USSD)",
		"QuectelSendUSSD(Device,USSD)\n"
		"  Device   - Id of device from quectel5g.conf\n"
		"  USSD     - ussd command\n"
	}
};

#if ASTERISK_VERSION_NUM >= 10800 /* 1.8+ */
typedef int (*app_func_t)(struct ast_channel *channel, const char *data);
#else /* 1.8- */
typedef int (*app_func_t)(struct ast_channel *channel, void *data);
#endif /* ^1.8- */

#/* */
EXPORT_DEF void app_register()
{
	unsigned i;
	for(i = 0; i < ITEMS_OF(dca); i++)
	{
		ast_register_application2 (dca[i].name, (app_func_t)(dca[i].func), dca[i].synopsis, dca[i].desc, self_module());
	}
}

#/* */
EXPORT_DEF void app_unregister()
{
	int i;
	for(i = ITEMS_OF(dca)-1; i >= 0; i--)
	{
		ast_unregister_application(dca[i].name);
	}
}

#endif /* BUILD_APPLICATIONS */
