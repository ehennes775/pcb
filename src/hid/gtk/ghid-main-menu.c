/*! \file <ghid-main-menu.c>
 *  \brief Implementation of GHidMainMenu widget
 *  \par Description
 *  This widget is the main pcb menu.
 */

#include <assert.h>

#include <error.h>

#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>

#include "gtkhid.h"
#include "gui.h"
#include "pcb-printf.h"

#include "ghid-main-menu.h"
#include "ghid-layer-selector.h"
#include "ghid-route-style-selector.h"

void Message (const char *, ...);

static int action_counter;

struct _GHidMainMenu
{
  GtkMenuBar parent;

  GtkActionGroup *action_group;
  GtkAccelGroup *accel_group;

  gint layer_view_pos;
  gint layer_pick_pos;
  gint route_style_pos;

  GtkMenuShell *layer_view_shell;
  GtkMenuShell *layer_pick_shell;
  GtkMenuShell *route_style_shell;

  GList *actions;
  GHashTable *popup_table;

  gint n_layer_views;
  gint n_layer_picks;
  gint n_route_styles;

  GCallback action_cb;
  void (*special_key_cb) (const char *accel, GtkAction *action,
                          const Resource *node);
};

struct _GHidMainMenuClass
{
  GtkMenuBarClass parent_class;
};

/* TODO: write finalize function */

/* SIGNAL HANDLERS */

/* RESOURCE HANDLER */
/* \brief Translate gpcb-menu.res accelerators to gtk ones
 * \par Function Description
 * Some keys need to be replaced by a name for the gtk accelerators to
 * work.  This table contains the translations.  The "in" character is
 * what would appear in gpcb-menu.res and the "out" string is what we
 * have to feed to gtk.  I was able to find these by using xev to find
 * the keycode and then looked at gtk+-2.10.9/gdk/keynames.txt (from the
 * gtk source distribution) to figure out the names that go with the 
 * codes.
 */
static gchar *
translate_accelerator (const char *text)
{
  GString *ret_val = g_string_new ("");
  static struct { const char *in, *out; } key_table[] = 
  {
    {"Enter", "Return"},
    {"Alt",   "<alt>"},
    {"Shift", "<shift>"},
    {"Ctrl",  "<ctrl>"},
    {" ", ""},
    {":", "colon"},
    {"=", "equal"},
    {"/", "slash"},
    {"[", "bracketleft"},
    {"]", "bracketright"},
    {".", "period"},
    {"|", "bar"},
    {"+", "plus"},
    {"-", "minus"},
    {NULL, NULL}
  };

  enum {MOD, KEY} state = MOD;
  while (*text != '\0')
    {
      static gboolean gave_msg;
      gboolean found = FALSE;
      int i;

      if (state == MOD && strncmp (text, "<Key>", 5) == 0)
        {
          state = KEY;
          text += 5;
        }
      for (i = 0; key_table[i].in != NULL; ++i)
        {
          int len = strlen (key_table[i].in);
          if (strncmp (text, key_table[i].in, len) == 0)
            {
              found = TRUE;
              g_string_append (ret_val, key_table[i].out);
              text += len;
            }
        }
      if (found == FALSE)
        switch (state)
          {
          case MOD:
            Message (_("Don't know how to parse \"%s\" as an "
                       "accelerator in the menu resource file.\n"),
                     text);
            if (!gave_msg)
              {
                gave_msg = TRUE;
                Message (_("Format is:\n"
                           "modifiers<Key>k\n"
                           "where \"modifiers\" is a space "
                           "separated list of key modifiers\n"
                           "and \"k\" is the name of the key.\n"
                           "Allowed modifiers are:\n"
                           "   Ctrl\n"
                           "   Shift\n"
                           "   Alt\n"
                           "Please note that case is important.\n"));
              }
            break;
          case KEY:
            g_string_append_c (ret_val, *text);
            ++text;
            break;
          }
    }
  return g_string_free (ret_val, FALSE);
}

static gboolean
g_str_case_equal (gconstpointer v1, gconstpointer v2)
{
  return strcasecmp (v1, v2);
}

/*! \brief Check that translated accelerators are unique; warn otherwise. */
static const char *
check_unique_accel (const char *accelerator)
{
  static GHashTable *accel_table;

  if (!accelerator ||*accelerator)
    return accelerator;

  if (!accel_table)
    accel_table = g_hash_table_new (g_str_hash, g_str_case_equal);

  if (g_hash_table_lookup (accel_table, accelerator))
    {
       Message (_("Duplicate accelerator found: \"%s\"\n"
                  "The second occurance will be dropped\n"),
                accelerator);
        return NULL;
    }

  g_hash_table_insert (accel_table,
                       (gpointer) accelerator, (gpointer) accelerator);

  return accelerator;
}

/* Die with a source location reference and err_message if cond isn't met.  */
#define REQUIRE(cond, err_message, ...) \
  do {                                  \
    if ( ! (cond) ) {                   \
      fprintf (                         \
          stderr,                       \
          "%s:%d: " err_message,        \
          __FILE__,                     \
          __LINE__,                     \
          __VA_ARGS__);                 \
      exit (EXIT_FAILURE);              \
    }                                   \
  } while ( 0 )

/* Find the first string of the form ref:anchor:property (where ref: is
 * literal and anchor and property are composed of isalnum() characters
 * or underscores) in str.  Return strdup()s of the anchor and property in
 * *anchor and *property, A pointer to the first character of the ref in str
 * in *start, and a pointer to the first character after the end of property
 * as the result.  NULL is returned for everything if a ref isn't found.
 * Malformed refs trigger a fatal error.  */
static char *
parse_first_ref (char const *str, char **start, char **anchor, char **property)
{
  /* Find the start of the ref */
  *start = strstr (str, "ref:");

  /* If we didn't find any refs we do nothing and return all NULLs. */
  if ( *start == NULL ) {
    *anchor = NULL;
    *property = NULL;
    return NULL;
  }

  /* Get copies of the anchor and property parts of the ref */
  *anchor = strdup (*start + strlen ("ref:"));
  *property = strstr (*anchor, ":");
  REQUIRE (
      *property != NULL,
      "Malformed ref:  missing property part: near start of '%s'\n",
      *start );
  **property = '\0';
  (*property)++;
  *property = strdup (*property);
  char *temp;
  for ( temp = *property ; isalnum (*temp) || *temp == '_' ; temp++ ) {
    ; 
  }
  *temp = '\0';
  REQUIRE (
      strlen (*anchor) != 0, 
      "Malformed ref: zero-length anchor part: near start of '%s'\n",
      *start );
  REQUIRE (
      strlen (*property) != 0, 
      "Malformed ref: zero-length property part: near start of '%s'\n",
      *start );

  /* Return pointer to character after end of property */
  return (
      *start +
      strlen("ref:") +
      strlen (*anchor) +
      strlen (":") +
      strlen (*property) );
}

/* If str is non-NULL, return a new GString consisting of str with all
 * ref:anchor:property strings expanded using the Resource pointers in
 * anchors, otherwise return an empty GString.  If anchors is NULL it is
 * treated as an empty table.  */
static GString * 
expand_refs (char const *str, GHashTable *anchors)
{
  GString *result = g_string_new ("");

  if ( str == NULL ) {
    return result;
  }

  if ( anchors == NULL ) {
    g_string_append (result, str);
    return result;
  }

  char *start, *anchor, *property, *rest = (char *) str;

  char const *part_before = str;

  while ( (rest = parse_first_ref (rest, &start, &anchor, &property)) ) {
      
    g_string_append_len (result, part_before, start - part_before);

    Resource *res = g_hash_table_lookup (anchors, anchor);

    REQUIRE (res != NULL, "Reference to non-existent anchor '%s'\n", anchor);

    if ( strcmp (property, "hotkey") == 0 ) {

      /* Find the accelerator ("a") sub-resource */
      Resource *ar = NULL;
      for ( int ii = 0 ; ii < res->c ; ii++ ) {
        char *field_name = ((res->v)[ii]).name;
        if ( field_name != NULL && strcmp (field_name, "a") == 0 ) {
          ar = ((res->v)[ii]).subres;
          break;
        }
      }
      REQUIRE (
          ar != NULL,
          "Reference 'ref:%s:%s' is invalid because the resource containing "
          "anchor '%s' doesn't also contain an accelerator sub-resource\n",
          anchor, property, anchor );
      /* The human-readable HotKey Name is first value of "a" sub-resource */
      char *hkn = (ar->v[0]).value;

      g_string_append (result, hkn);
    }

    else if ( strcmp (property, "menu_path") == 0 ) {

      GString *path = g_string_new ("");
      Resource *cr = res;   /* Current Resource */
      /* Traverse up.  Menu item Resource entries have the menu item name
       * as their first value, but top-level resources have a sub-resource
       * as their first value, so when we get to one of those we're done.  */
      do {
        g_string_prepend (path, cr->v[0].value);
        g_string_prepend (path, "->");
        cr = cr->parent;
      } while ( cr != NULL && (cr->v[0].value != NULL) );
      assert (cr->v[0].subres != NULL);

      g_string_erase (path, 0, strlen ("->"));   /* Erase leading "->" */

      g_string_append (result, path->str);

      g_string_free (path, TRUE);
    }

    else {

      REQUIRE (false,  "Unknown property name '%s'\n", property);

    }

    part_before = rest;

    free (property);
    free (anchor);
  }

  /* Note that because there din't turn out to be any more refs, the
   * part_before now actually consists of everything from the point after
   * the last ref to the end of the string.  */
  g_string_append (result, part_before);

  return result;
}

/*! \brief Translate a resource tree into a menu structure
 *
 *  \param [in] menu    The GHidMainMenu widget to be acted on
 *  \param [in] shall   The base menu shell (a menu bar or popup menu)
 *  \param [in] res     The base of the resource tree
 * */
void
ghid_main_menu_real_add_resource (GHidMainMenu *menu, GtkMenuShell *shell,
                                  const Resource *res)
{
  int i, j;
  const Resource *tmp_res;
  gchar mnemonic = 0;

  /* Note that the anchors table should have all anchors associate with the
   * top-level parent of the current resource.  */
  GHashTable *anchors = res->anchors;

  for (i = 0; i < res->c; ++i)
    {
      const gchar *accel = NULL;
      char *menu_label;
      const char *res_val;
      const Resource *sub_res = res->v[i].subres;
      GtkAction *action = NULL;

      switch (resource_type (res->v[i]))
        {
        case 101:   /* name, subres: passthrough */
          ghid_main_menu_real_add_resource (menu, shell, sub_res);
          break;
        case   1:   /* no name, subres */
          tmp_res = resource_subres (sub_res, "a");  /* accelerator */
          res_val = resource_value (sub_res, "m");   /* mnemonic */
          if (res_val)
            mnemonic = res_val[0];
          /* The accelerator resource will have two values, like 
           *   a={"Ctrl-Q" "Ctrl<Key>q"}
           * The first Gtk ignores. The second needs to be translated. */
          if (tmp_res)
            accel = check_unique_accel
                      (translate_accelerator (tmp_res->v[1].value));

          /* Now look for the first unnamed value (not a subresource) to
           * figure out the name of the menu or the menuitem. */
          res_val = "button";
          for (j = 0; j < sub_res->c; ++j)
            if (resource_type (sub_res->v[j]) == 10)
              {
                res_val = _(sub_res->v[j].value);
                break;
              }
          /* Hack '_' in based on mnemonic value */
          if (!mnemonic)
            menu_label = g_strdup (res_val);
          else
            {
              char *post_ = strchr (res_val, mnemonic);
              if (post_ == NULL)
                menu_label = g_strdup (res_val);
              else
                {
                  GString *tmp = g_string_new ("");
                  g_string_append_len (tmp, res_val, post_ - res_val);
                  g_string_append_c (tmp, '_');
                  g_string_append (tmp, post_);
                  menu_label = g_string_free (tmp, FALSE);
                }
            }
          /* If the subresource we're processing also has unnamed
           * subresources, it's a submenu, not a regular menuitem. */
          if (sub_res->flags & FLAG_S)
            {
              /* SUBMENU */
              GtkWidget *submenu = gtk_menu_new ();
              GtkWidget *item = gtk_menu_item_new_with_mnemonic (menu_label);
              GtkWidget *tearoff = gtk_tearoff_menu_item_new ();
              const char *tip = resource_value (sub_res, "tip");
              GString *tipwer = expand_refs (tip, anchors);

              gtk_menu_shell_append (shell, item);
              gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), submenu);

              gtk_widget_set_tooltip_text (item, tipwer->str);

              g_string_free (tipwer, TRUE);

              /* add tearoff to menu */
              gtk_menu_shell_append (GTK_MENU_SHELL (submenu), tearoff);
              /* recurse on the newly-added submenu */
              ghid_main_menu_real_add_resource (menu,
                                                GTK_MENU_SHELL (submenu),
                                                sub_res);
            }
          else
            {
              /* NON-SUBMENU: MENU ITEM */
              const char *checked = resource_value (sub_res, "checked");
              const char *label = resource_value (sub_res, "sensitive");
              const char *tip = resource_value (sub_res, "tip");
              /* Tip With Expanded Refs  */
              GString *tipwer = expand_refs (tip, anchors);
              if (checked)
                {
                  /* TOGGLE ITEM */
                  gchar *name = g_strdup_printf ("MainMenuAction%d",
                                                 action_counter++);
                  action
                    = GTK_ACTION ( gtk_toggle_action_new (name, menu_label,
                                                          tipwer->str, NULL));
                  /* checked=foo       is a binary flag (checkbox)
                   * checked=foo,bar   is a flag compared to a value (radio) */
                  gtk_toggle_action_set_draw_as_radio
                    (GTK_TOGGLE_ACTION (action), !!strchr (checked, ','));
                }
              else if (label && strcmp (label, "false") == 0)
                {
                  /* INSENSITIVE ITEM */
                  GtkWidget *item = gtk_menu_item_new_with_label (menu_label);
                  gtk_widget_set_sensitive (item, FALSE);
                  gtk_menu_shell_append (shell, item);
                  gtk_widget_set_tooltip_text (item, tipwer->str);
                }
              else
                {
                  /* NORMAL ITEM */
                  gchar *name
                    = g_strdup_printf ("MainMenuAction%d", action_counter++);
                  action
                    = gtk_action_new (name, menu_label, tipwer->str, NULL);
                }
              g_string_free (tipwer, TRUE);
            }
          /* If this menu item has an associated, create and set up it's
           * widget.  */
          if (action)
            {
              GtkWidget *item;
              gtk_action_set_accel_group (action, menu->accel_group);
              gtk_action_group_add_action_with_accel (menu->action_group,
                                                      action, accel);
              gtk_action_connect_accelerator (action);
              g_signal_connect (G_OBJECT (action), "activate", menu->action_cb,
                                (gpointer) sub_res);
              g_object_set_data (G_OBJECT (action), "resource",
                                 (gpointer) sub_res);
              item = gtk_action_create_menu_item (action);
              gtk_menu_shell_append (shell, item);
              menu->actions = g_list_append (menu->actions, action);
              menu->special_key_cb (accel, action, sub_res);
              /* For some reason passing the tip to the GtkAction associated
               * with this widget isn't sufficient to make the tooltip actually
               * display, so we do this as well.  */
              gtk_widget_set_tooltip_text (
                  item,
                  gtk_action_get_tooltip (action) );
            }
          /* Scan rest of resource in case there is more work */
          for (j = 0; j < sub_res->c; j++)
            {
              const char *res_name;
              /* named value = X resource */
              if (resource_type (sub_res->v[j]) == 110)
                {
                  res_name = sub_res->v[j].name;

                  /* translate bg, fg to background, foreground */
                  if (strcmp (res_name, "fg") == 0)   res_name = "foreground";
                  if (strcmp (res_name, "bg") == 0)   res_name = "background";

                  /* ignore special named values (m, a, sensitive) */
                  if (strcmp (res_name, "m") == 0
                      || strcmp (res_name, "a") == 0
                      || strcmp (res_name, "sensitive") == 0
                      || strcmp (res_name, "tip") == 0)
                    break;

                  /* log checked and active special values */
                  if (action && strcmp (res_name, "checked") == 0)
                    g_object_set_data (G_OBJECT (action), "checked-flag",
                                       sub_res->v[j].value);
                  else if (action && strcmp (res_name, "active") == 0)
                    g_object_set_data (G_OBJECT (action), "active-flag",
                                       sub_res->v[j].value);
                  else
                    /* if we got this far it is supposed to be an X
                     * resource.  For now ignore it and warn the user */
                    Message (_("The gtk gui currently ignores \"%s\""
                               "as part of a menuitem resource.\n"
                               "Feel free to provide patches\n"),
                             sub_res->v[j].value);
                }
            }
          break;
        case  10:   /* no name, value */
          /* If we get here, the resource is "-" or "@foo" for some foo */
          if (res->v[i].value[0] == '@')
            {
              GList *children;
              int pos;

              children = gtk_container_get_children (GTK_CONTAINER (shell));
              pos = g_list_length (children);
              g_list_free (children);

              if (strcmp (res->v[i].value, "@layerview") == 0)
                {
                  menu->layer_view_shell = shell;
                  menu->layer_view_pos = pos;
                }
              else if (strcmp (res->v[i].value, "@layerpick") == 0)
                {
                  menu->layer_pick_shell = shell;
                  menu->layer_pick_pos = pos;
                }
              else if (strcmp (res->v[i].value, "@routestyles") == 0)
                {
                  menu->route_style_shell = shell;
                  menu->route_style_pos = pos;
                }
              else
                Message (_("GTK GUI currently ignores \"%s\" in the menu\n"
                           "resource file.\n"), res->v[i].value);
            }
          else if (strcmp (res->v[i].value, "-") == 0)
            {
              GtkWidget *item = gtk_separator_menu_item_new ();
              gtk_menu_shell_append (shell, item);
            }
          else if (i > 0)
            {
              /* This is an action-less menuitem. It is really only useful
               * when you're starting to build a new menu and you're looking
               * to get the layout right. */
              GtkWidget *item
                = gtk_menu_item_new_with_label (_(res->v[i].value));
              gtk_menu_shell_append (shell, item);
            }
          break;
      }
  }
}

/* CONSTRUCTOR */
static void
ghid_main_menu_init (GHidMainMenu *mm)
{
  /* Hookup signal handlers */
}

static void
ghid_main_menu_class_init (GHidMainMenuClass *klass)
{
}

/* PUBLIC FUNCTIONS */
GType
ghid_main_menu_get_type (void)
{
  static GType mm_type = 0;

  if (!mm_type)
    {
      const GTypeInfo mm_info =
        {
          sizeof (GHidMainMenuClass),
          NULL, /* base_init */
          NULL, /* base_finalize */
          (GClassInitFunc) ghid_main_menu_class_init,
          NULL, /* class_finalize */
          NULL, /* class_data */
          sizeof (GHidMainMenu),
          0,    /* n_preallocs */
          (GInstanceInitFunc) ghid_main_menu_init,
        };

      mm_type = g_type_register_static (GTK_TYPE_MENU_BAR,
                                        "GHidMainMenu",
                                        &mm_info, 0);
    }

  return mm_type;
}

/*! \brief Create a new GHidMainMenu
 *
 *  \return a freshly-allocated GHidMainMenu
 */
GtkWidget *
ghid_main_menu_new (GCallback action_cb,
                    void (*special_key_cb) (const char *accel,
                                            GtkAction *action,
                                            const Resource *node))
{
  GHidMainMenu *mm = g_object_new (GHID_MAIN_MENU_TYPE, NULL);

  mm->accel_group = gtk_accel_group_new ();
  mm->action_group = gtk_action_group_new ("MainMenu");

  mm->layer_view_pos = 0;
  mm->layer_pick_pos = 0;
  mm->route_style_pos = 0;
  mm->n_layer_views = 0;
  mm->n_layer_picks = 0;
  mm->n_route_styles = 0;
  mm->layer_view_shell = NULL;
  mm->layer_pick_shell = NULL;
  mm->route_style_shell = NULL;

  mm->special_key_cb = special_key_cb;
  mm->action_cb = action_cb;
  mm->actions = NULL;
  mm->popup_table = g_hash_table_new (g_str_hash, g_str_equal);

  return GTK_WIDGET (mm);
}

/*! \brief Turn a pcb resource into the main menu */
void
ghid_main_menu_add_resource (GHidMainMenu *menu, const Resource *res)
{
  ghid_main_menu_real_add_resource (menu, GTK_MENU_SHELL (menu), res);
}

/*! \brief Turn a pcb resource into a popup menu */
void
ghid_main_menu_add_popup_resource (GHidMainMenu *menu, const char *name,
                                   const Resource *res)
{
  GtkWidget *new_menu = gtk_menu_new ();
  g_object_ref_sink (new_menu);
  ghid_main_menu_real_add_resource (menu, GTK_MENU_SHELL (new_menu), res);
  g_hash_table_insert (menu->popup_table, (gpointer) name, new_menu);
  gtk_widget_show_all (new_menu);
}

/*! \brief Returns a registered popup menu by name */
GtkMenu *
ghid_main_menu_get_popup (GHidMainMenu *menu, const char *name)
{
  return g_hash_table_lookup (menu->popup_table, name);
}


/*! \brief Updates the toggle/active state of all items 
 *  \par Function Description
 *  Loops through all actions, passing the action, its toggle
 *  flag (maybe NULL), and its active flag (maybe NULL), to a
 *  callback function. It is the responsibility of the function
 *  to actually change the state of the action.
 *
 *  \param [in] menu    The menu to be acted on.
 *  \param [in] cb      The callback that toggles the actions
 */
void
ghid_main_menu_update_toggle_state (GHidMainMenu *menu,
                                    void (*cb) (GtkAction *,
                                                const char *toggle_flag,
                                                const char *active_flag))
{
  GList *list;
  for (list = menu->actions; list; list = list->next)
    {
      Resource *res = g_object_get_data (G_OBJECT (list->data), "resource");
      const char *tf = g_object_get_data (G_OBJECT (list->data),
                                          "checked-flag");
      const char *af = g_object_get_data (G_OBJECT (list->data),
                                          "active-flag");
      g_signal_handlers_block_by_func (G_OBJECT (list->data),
                                       menu->action_cb, res);
      cb (GTK_ACTION (list->data), tf, af);
      g_signal_handlers_unblock_by_func (G_OBJECT (list->data),
                                         menu->action_cb, res);
    }
}

/*! \brief Installs or updates layer selector items */
void
ghid_main_menu_install_layer_selector (GHidMainMenu *mm,
                                       GHidLayerSelector *ls)
{
  GList *children, *iter;

  /* @layerview */
  if (mm->layer_view_shell)
    {
      /* Remove old children */
      children = gtk_container_get_children
                   (GTK_CONTAINER (mm->layer_view_shell));
      for (iter = g_list_nth (children, mm->layer_view_pos);
           iter != NULL && mm->n_layer_views > 0;
           iter = g_list_next (iter), mm->n_layer_views --)
        gtk_container_remove (GTK_CONTAINER (mm->layer_view_shell),
                              iter->data);
      g_list_free (children);

      /* Install new ones */
      mm->n_layer_views = ghid_layer_selector_install_view_items
                            (ls, mm->layer_view_shell, mm->layer_view_pos);
    }

  /* @layerpick */
  if (mm->layer_pick_shell)
    {
      /* Remove old children */
      children = gtk_container_get_children
                   (GTK_CONTAINER (mm->layer_pick_shell));
      for (iter = g_list_nth (children, mm->layer_pick_pos);
           iter != NULL && mm->n_layer_picks > 0;
           iter = g_list_next (iter), mm->n_layer_picks --)
        gtk_container_remove (GTK_CONTAINER (mm->layer_pick_shell),
                              iter->data);
      g_list_free (children);

      /* Install new ones */
      mm->n_layer_picks = ghid_layer_selector_install_pick_items
                            (ls, mm->layer_pick_shell, mm->layer_pick_pos);
    }
}

/*! \brief Installs or updates route style selector items */
void
ghid_main_menu_install_route_style_selector (GHidMainMenu *mm,
                                             GHidRouteStyleSelector *rss)
{
  GList *children, *iter;
  /* @routestyles */
  if (mm->route_style_shell)
    {
      /* Remove old children */
      children = gtk_container_get_children
                   (GTK_CONTAINER (mm->route_style_shell));
      for (iter = g_list_nth (children, mm->route_style_pos);
           iter != NULL && mm->n_route_styles > 0;
           iter = g_list_next (iter), mm->n_route_styles --)
        gtk_container_remove (GTK_CONTAINER (mm->route_style_shell),
                              iter->data);
      g_list_free (children);
      /* Install new ones */
      mm->n_route_styles = ghid_route_style_selector_install_items
                             (rss, mm->route_style_shell, mm->route_style_pos);
    }
}

/*! \brief Returns the menu bar's accelerator group */
GtkAccelGroup *
ghid_main_menu_get_accel_group (GHidMainMenu *menu)
{
  return menu->accel_group;
}

