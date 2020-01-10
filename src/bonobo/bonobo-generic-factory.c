/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * bonobo-generic-factory.c: a GenericFactory object.
 *
 * The BonoboGenericFactory object is used to instantiate new
 * Bonobo::GenericFactory objects.  It acts as a wrapper for the
 * Bonobo::GenericFactory CORBA interface, and dispatches to
 * a user-specified factory function whenever its create_object()
 * method is invoked.
 *
 * Author:
 *   Miguel de Icaza (miguel@kernel.org)
 *   �RDI Gerg� (cactus@cactus.rulez.org): cleanup
 *
 * Copyright 1999, 2001 Ximian, Inc., 2001 Gerg� �rdi
 */
#include <config.h>
#include <string.h>
#include <glib-object.h>
#include <bonobo/Bonobo.h>
#include <bonobo/bonobo-main.h>
#include <bonobo/bonobo-types.h>
#include <bonobo/bonobo-marshal.h>
#include <bonobo/bonobo-exception.h>
#include <bonobo/bonobo-generic-factory.h>
#include <bonobo/bonobo-context.h>
#include <bonobo/bonobo-running-context.h>
#include <bonobo/bonobo-debug.h>
#include "bonobo-private.h"

#define DEFAULT_LAST_UNREF_TIMEOUT 	2000
#define STARTUP_TIMEOUT 		60

struct _BonoboGenericFactoryPrivate {
	/* The function factory */
	GClosure *factory_closure;
	/* The component_id for this generic factory */
	char     *act_iid;
	/* Whether we should register with the activation server */
	gboolean  noreg;
	  /* Startup timeout when no object is ever requested */
	guint startup_timeout_id;
	  /* Timeout after last unref before quitting */
	guint last_unref_timeout_id;
	guint last_unref_timeout; /* miliseconds */
	guint last_unref_ignore; /* ignore last-unref caused by temporary objects */
	gboolean last_unref_set; /* keep track of last-unref status during "ignore" period */
};

static GObjectClass *bonobo_generic_factory_parent_class = NULL;

static CORBA_Object
impl_Bonobo_ObjectFactory_createObject (PortableServer_Servant   servant,
					const CORBA_char        *obj_act_iid,
					CORBA_Environment       *ev)
{
	BonoboGenericFactoryClass *class;
	BonoboGenericFactory      *factory;
	BonoboObject              *object;

	factory = BONOBO_GENERIC_FACTORY (bonobo_object (servant));

	class = BONOBO_GENERIC_FACTORY_CLASS (G_OBJECT_GET_CLASS (factory));

	BONOBO_LOCK();
	  /* Cancel the startup timeout, if active. */
	if (factory->priv->startup_timeout_id) {
	    g_source_destroy (g_main_context_find_source_by_id
			      (NULL, factory->priv->startup_timeout_id));
	    factory->priv->startup_timeout_id = 0;
	}

	  /* Cancel the last unref timeout, if active. */
	if (factory->priv->last_unref_timeout_id) {
	    g_source_destroy (g_main_context_find_source_by_id
			      (NULL, factory->priv->last_unref_timeout_id));
	    factory->priv->last_unref_timeout_id = 0;
	}
	BONOBO_UNLOCK();

	object = (*class->new_generic) (factory, obj_act_iid);
	factory = NULL; /* unreffed by new_generic in the shlib case */

	if (!object)
		return CORBA_OBJECT_NIL;
	
	return CORBA_Object_duplicate (BONOBO_OBJREF (object), ev);
}

/**
 * bonobo_generic_factory_construct_noreg:
 * @factory: The object to be initialized.
 * @act_iid: The GOAD id that the new factory will implement.
 * @factory_closure: A Multi object factory closure.
 *
 * Initializes @c_factory with the supplied closure and iid.
 */
void
bonobo_generic_factory_construct_noreg (BonoboGenericFactory   *factory,
					const char             *act_iid,
					GClosure               *factory_closure)
{
	g_return_if_fail (BONOBO_IS_GENERIC_FACTORY (factory));
	
	factory->priv->act_iid = g_strdup (act_iid);
	factory->priv->noreg   = TRUE;

	if (factory_closure)
		factory->priv->factory_closure =
			bonobo_closure_store (factory_closure, bonobo_marshal_OBJECT__STRING);
}

/**
 * bonobo_generic_factory_construct:
 * @factory: The object to be initialized.
 * @act_iid: The Bonobo activation id that the new factory will implement.
 * Bonobo::GenericFactory interface and which will be used to
 * construct this BonoboGenericFactory Gtk object.
 * @factory_closure: A Multi object factory closure.
 *
 * Initializes @c_factory with and registers the new factory with
 * the name server.
 *
 * Returns: The initialized BonoboGenericFactory object or NULL
 *          if already registered.
 */
BonoboGenericFactory *
bonobo_generic_factory_construct (BonoboGenericFactory   *factory,
				  const char             *act_iid,
				  GClosure               *factory_closure)
{
	int ret;

	g_return_val_if_fail (BONOBO_IS_GENERIC_FACTORY (factory), NULL);
	
	bonobo_generic_factory_construct_noreg (factory, act_iid, factory_closure);

	factory->priv->noreg = FALSE;

	ret = bonobo_activation_active_server_register (act_iid, BONOBO_OBJREF (factory));

	if (ret != Bonobo_ACTIVATION_REG_SUCCESS) {
		bonobo_object_unref (BONOBO_OBJECT (factory));
#ifdef G_ENABLE_DEBUG
		if (_bonobo_debug_flags & BONOBO_DEBUG_LIFECYCLE) {
			const char *err;
			switch (ret)
			{
			case Bonobo_ACTIVATION_REG_SUCCESS:        err = "success";        break;
			case Bonobo_ACTIVATION_REG_NOT_LISTED:     err = "not listed";     break;
			case Bonobo_ACTIVATION_REG_ALREADY_ACTIVE: err = "already active"; break;
			case Bonobo_ACTIVATION_REG_ERROR: 	   err = "error";          break;
			default: err = "(invalid error!)"; break;
			}
			g_warning ("'%s' factory registration failed: %s",
				   act_iid, err);
		}
#endif
		return NULL;
	}

	return factory;
}

/**
 * bonobo_generic_factory_new_closure:
 * @act_iid: The GOAD id that this factory implements
 * @factory_closure: A closure which is used to create new BonoboObject instances.
 *
 * This is a helper routine that simplifies the creation of factory
 * objects for GNOME objects.  The @factory_closure closure will be
 * invoked by the CORBA server when a request arrives to create a new
 * instance of an object supporting the Bonobo::Generic interface.
 * The factory callback routine is passed the @data pointer to provide
 * the creation function with some state information.
 *
 * Returns: A BonoboGenericFactory object that has an activated
 * Bonobo::GenericFactory object that has registered with the GNOME
 * name server.
 */
BonoboGenericFactory *
bonobo_generic_factory_new_closure (const char *act_iid,
				    GClosure   *factory_closure)
{
	BonoboGenericFactory *factory;

	g_return_val_if_fail (act_iid != NULL, NULL);
	g_return_val_if_fail (factory_closure != NULL, NULL);
	
	factory = g_object_new (bonobo_generic_factory_get_type (), NULL);

	return bonobo_generic_factory_construct (
		factory, act_iid, factory_closure);
}


/**
 * bonobo_generic_factory_new:
 * @act_iid: The GOAD id that this factory implements
 * @factory_cb: A callback which is used to create new BonoboObject instances.
 * @user_data: The closure data to be passed to the @factory callback routine.
 *
 * This is a helper routine that simplifies the creation of factory
 * objects for GNOME objects.  The @factory function will be
 * invoked by the CORBA server when a request arrives to create a new
 * instance of an object supporting the Bonobo::Generic interface.
 * The factory callback routine is passed the @data pointer to provide
 * the creation function with some state information.
 *
 * Returns: A BonoboGenericFactory object that has an activated
 * Bonobo::GenericFactory object that has registered with the GNOME
 * name server.
 */
BonoboGenericFactory *
bonobo_generic_factory_new (const char           *act_iid,
			    BonoboFactoryCallback factory_cb,
			    gpointer              user_data)
{
	return bonobo_generic_factory_new_closure (
		act_iid, g_cclosure_new (G_CALLBACK (factory_cb), user_data, NULL));
}

static void
bonobo_generic_factory_destroy (BonoboObject *object)
{
	BonoboGenericFactory *factory = BONOBO_GENERIC_FACTORY (object);

	if (factory->priv) {
		if (!factory->priv->noreg && factory->priv->act_iid)
			bonobo_activation_active_server_unregister (
				factory->priv->act_iid, BONOBO_OBJREF (factory));

		g_free (factory->priv->act_iid);

		if (factory->priv->factory_closure)
			g_closure_unref (factory->priv->factory_closure);

		  /* Cancel the startup timeout, if active. */
		if (factory->priv->startup_timeout_id)
			g_source_destroy (g_main_context_find_source_by_id
					  (NULL, factory->priv->startup_timeout_id));
		  /* Cancel the last unref timeout, if active. */
		if (factory->priv->last_unref_timeout_id)
			g_source_destroy (g_main_context_find_source_by_id
					  (NULL, factory->priv->last_unref_timeout_id));
		
		g_free (factory->priv);
		factory->priv = NULL;
	}
		
	BONOBO_OBJECT_CLASS (bonobo_generic_factory_parent_class)->destroy (object);
}

static gboolean
last_unref_timeout (gpointer data)
{
	BonoboGenericFactory *factory = BONOBO_GENERIC_FACTORY (data);
	bonobo_main_quit ();
	factory->priv->last_unref_timeout_id = 0;
	return FALSE;
}

static BonoboObject *
bonobo_generic_factory_new_generic (BonoboGenericFactory *factory,
				    const char           *act_iid)
{
	BonoboObject *ret;
	
	g_return_val_if_fail (factory != NULL, NULL);
	g_return_val_if_fail (BONOBO_IS_GENERIC_FACTORY (factory), NULL);

	BONOBO_LOCK();
	factory->priv->last_unref_ignore++;
	BONOBO_UNLOCK();

	bonobo_closure_invoke (factory->priv->factory_closure,
			       BONOBO_TYPE_OBJECT, &ret,
			       BONOBO_TYPE_GENERIC_FACTORY, factory,
			       G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE, act_iid, G_TYPE_INVALID);

	BONOBO_LOCK();
	factory->priv->last_unref_ignore--;

	if (ret) /* since we are returning a new object, even if
		  * last-unref was previously caught it no longer
		  * applies */
		factory->priv->last_unref_set = FALSE;

	if (factory->priv->last_unref_set) {
		factory->priv->last_unref_timeout_id = g_timeout_add
			(factory->priv->last_unref_timeout, last_unref_timeout, factory);
		factory->priv->last_unref_set = FALSE;
	}
	BONOBO_UNLOCK();

	return ret;
}

static void
bonobo_generic_factory_class_init (BonoboGenericFactoryClass *klass)
{
	BonoboObjectClass *bonobo_object_class = (BonoboObjectClass *) klass;
	POA_Bonobo_GenericFactory__epv *epv = &klass->epv;

	epv->createObject = impl_Bonobo_ObjectFactory_createObject;

	bonobo_object_class->destroy = bonobo_generic_factory_destroy;

	klass->new_generic = bonobo_generic_factory_new_generic;

	bonobo_generic_factory_parent_class = g_type_class_peek_parent (klass);
}

static void
bonobo_generic_factory_init (GObject *object)
{
	BonoboGenericFactory *factory = BONOBO_GENERIC_FACTORY (object);

	factory->priv = g_new0 (BonoboGenericFactoryPrivate, 1);
	factory->priv->noreg = FALSE;
	factory->priv->last_unref_timeout = DEFAULT_LAST_UNREF_TIMEOUT;
}

BONOBO_TYPE_FUNC_FULL (BonoboGenericFactory, 
		       Bonobo_GenericFactory,
		       BONOBO_TYPE_OBJECT,
		       bonobo_generic_factory)


static gboolean
startup_timeout (gpointer data)
{
	BonoboGenericFactory *factory = BONOBO_GENERIC_FACTORY (data);
	bonobo_main_quit ();
	factory->priv->startup_timeout_id = 0;
	return FALSE;
}

static void
last_unref_cb (BonoboRunningContext *context, BonoboGenericFactory *factory)
{
	g_return_if_fail (BONOBO_IS_GENERIC_FACTORY (factory));
	if (factory->priv->last_unref_ignore) {
		factory->priv->last_unref_set = TRUE;
		return;
	}
	g_return_if_fail (!factory->priv->last_unref_timeout_id);
	factory->priv->last_unref_timeout_id = g_timeout_add
		(factory->priv->last_unref_timeout, last_unref_timeout, factory);
}

/**
 * bonobo_generic_factory_main:
 * @act_iid: the oaf iid of the factory
 * @factory_cb: the factory callback
 * @user_data: a user data pointer
 * 
 *    A Generic 'main' routine so we don't stick a load of code
 * inside a public macro. See also bonobo_generic_factory_main_timeout().
 * 
 * Return value: 0 on success, 1 on failure.
 **/
int
bonobo_generic_factory_main (const char           *act_iid,
			     BonoboFactoryCallback factory_cb,
			     gpointer              user_data)
{
	return bonobo_generic_factory_main_timeout
		(act_iid, factory_cb, user_data, DEFAULT_LAST_UNREF_TIMEOUT);
}

/**
 * bonobo_generic_factory_main_timeout:
 * @act_iid: the oaf iid of the factory
 * @factory_cb: the factory callback
 * @user_data: a user data pointer
 * @quit_timeout: ammount of time to wait (miliseconds) after all
 * objects have been released before quitting the main loop.
 * 
 *    A Generic 'main' routine so we don't stick a load of code
 * inside a public macro.
 * 
 * Return value: 0 on success, 1 on failure.
 **/
int
bonobo_generic_factory_main_timeout (const char           *act_iid,
				     BonoboFactoryCallback factory_cb,
				     gpointer              user_data,
				     guint                 quit_timeout)
{
	BonoboGenericFactory *factory;

	factory = bonobo_generic_factory_new (
		act_iid, factory_cb, user_data);

	if (factory) {
		BonoboObject *context;
		guint         signal;

		factory->priv->last_unref_timeout = quit_timeout;
		context = bonobo_running_context_new ();
		signal = g_signal_connect (G_OBJECT (context), "last-unref",
					   G_CALLBACK (last_unref_cb), factory);
		bonobo_running_context_ignore_object (BONOBO_OBJREF (factory));

		  /* Create timeout here so if we haven't created anything
		   * within a few minutes we just quit */
		factory->priv->startup_timeout_id = g_timeout_add_seconds
			(STARTUP_TIMEOUT, startup_timeout, factory);

		bonobo_main ();

		g_signal_handler_disconnect (G_OBJECT (context), signal);
		bonobo_object_unref (BONOBO_OBJECT (factory));
		bonobo_object_unref (context);

		return bonobo_debug_shutdown ();
	} else
		return 1;
}

