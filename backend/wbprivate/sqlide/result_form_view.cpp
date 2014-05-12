/*
 * Copyright (c) 2013, 2014, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include "stdafx.h"
#include "wb_sql_editor_form.h"
#include "wb_sql_editor_result_panel.h"
#include "result_form_view.h"

#include "grtdb/db_helpers.h"
#include "grtui/inserts_export_form.h"
#include "sqlide/recordset_cdbc_storage.h"

#include "base/sqlstring.h"
#include "grt/parse_utils.h"
#include "base/log.h"
#include "base/boost_smart_ptr_helpers.h"

#include "mforms/record_grid.h"
#include "mforms/utilities.h"
#include "mforms/treenodeview.h"
#include "mforms/textbox.h"
#include "mforms/label.h"
#include "mforms/tabview.h"
#include "mforms/tabswitcher.h"
#include "mforms/menubar.h"

#include "mforms/button.h"
#include "mforms/selector.h"
#include "mforms/textentry.h"

#include <algorithm>

using namespace base;

DEFAULT_LOG_DOMAIN("SqlResult")

class ResultFormView::FieldView
{
  mforms::Label _label;

protected:

  boost::function<void (const std::string &s)> _change_callback;

public:
  FieldView(const std::string &name, const boost::function<void (const std::string &s)> &change_callback)
  : _label(name), _change_callback(change_callback)
  {
    _label.set_text_align(mforms::TopRight);
  }

  virtual ~FieldView() {}

  static ResultFormView::FieldView *create(const Recordset_cdbc_storage::FieldInfo &field, const std::string &full_type, bool editable,
                           const boost::function<void (const std::string &s)> &callback,
                           const boost::function<void ()> &view_blob_callback);

  mforms::Label *label() { return &_label; }
  virtual mforms::View *value() = 0;
  virtual bool expands() { return false; }

  virtual void set_value(const std::string &value, bool is_null) = 0;

};


class StringFieldView : public ResultFormView::FieldView
{
  mforms::TextEntry *_entry;
  bool _expands;

  void changed()
  {
    _change_callback(_entry->get_string_value());
  }

public:
  StringFieldView(const std::string &name, int max_length, bool editable, const boost::function<void (const std::string &s)> &change_callback)
  : FieldView(name, change_callback), _expands(false)
  {
    _entry = new mforms::TextEntry();
    _entry->set_enabled(editable);
    _entry->signal_changed()->connect(boost::bind(&StringFieldView::changed, this));
    if (max_length > 64)
      _expands = true;
    else
      _entry->set_size(std::max(max_length * 10, 60), -1);
  }

  virtual bool expands()
  {
    return _expands;
  }

  virtual ~StringFieldView()
  {
    _entry->release();
  }

  virtual mforms::View *value() { return _entry; }

  virtual void set_value(const std::string &value, bool is_null)
  {
    _entry->set_value(value);
  }
};


class SelectorFieldView : public ResultFormView::FieldView
{
  mforms::Selector *_selector;

  void changed()
  {
    _change_callback(_selector->get_string_value());
  }

public:
  SelectorFieldView(const std::string &name, const std::list<std::string> &items,
                    bool editable, const boost::function<void (const std::string &s)> &change_callback)
  : FieldView(name, change_callback)
  {
    _selector = new mforms::Selector();
    _selector->add_items(items);
    _selector->set_enabled(editable);
    _selector->signal_changed()->connect(boost::bind(&SelectorFieldView::changed, this));
  }

  virtual ~SelectorFieldView()
  {
    _selector->release();
  }

  virtual mforms::View *value() { return _selector; }

  virtual void set_value(const std::string &value, bool is_null)
  {
    _selector->set_value(value);
  }
};


class SetFieldView : public ResultFormView::FieldView
{
  mforms::TreeNodeView _tree;

  void changed()
  {
    std::string value;

    for (int c = _tree.count(), i = 0; i < c; i++)
    {
      mforms::TreeNodeRef node = _tree.node_at_row(i);
      if (node->get_bool(0))
      {
        if (!value.empty())
          value.append(",");
        value.append(node->get_string(1));
      }
    }
    _change_callback(value);
  }

public:
  SetFieldView(const std::string &name, const std::list<std::string> &items,
               bool editable, const boost::function<void (const std::string &s)> &change_callback)
  : FieldView(name, change_callback), _tree(mforms::TreeFlatList|mforms::TreeNoHeader)
  {
    _tree.add_column(mforms::CheckColumnType, "", 30, true);
    _tree.add_column(mforms::StringColumnType, "", 200, false);
    _tree.end_columns();

    for (std::list<std::string>::const_iterator i = items.begin(); i != items.end(); ++i)
    {
      mforms::TreeNodeRef node = _tree.add_node();
      node->set_string(1, *i);
    }

    int height = items.size() * 20;
    _tree.set_size(250, height > 100 ? 100 : height);

    _tree.set_enabled(editable);
    _tree.signal_changed()->connect(boost::bind(&SetFieldView::changed, this));
  }

  virtual mforms::View *value() { return &_tree; }

  virtual void set_value(const std::string &value, bool is_null)
  {
    std::vector<std::string> l(base::split_token_list(value, ','));

    for (int c = _tree.count(), i = 0; i < c; i++)
    {
      mforms::TreeNodeRef node = _tree.node_at_row(i);
      if (std::find(l.begin(), l.end(), node->get_string(1)) != l.end())
        node->set_bool(0, true);
      else
        node->set_bool(0, false);
    }
  }
};


class TextFieldView : public ResultFormView::FieldView
{
  mforms::TextBox *_tbox;

  void changed()
  {
    _change_callback(_tbox->get_string_value());
  }

public:
  TextFieldView(const std::string &name, bool editable, const boost::function<void (const std::string &s)> &change_callback)
  : FieldView(name, change_callback)
  {
    _tbox = new mforms::TextBox(mforms::BothScrollBars);
    _tbox->set_enabled(editable);
    _tbox->signal_changed()->connect(boost::bind(&TextFieldView::changed, this));
    _tbox->set_size(-1, 60);
  }

  virtual bool expands()
  {
    return true;
  }

  virtual ~TextFieldView()
  {
    _tbox->release();
  }

  virtual mforms::View *value() { return _tbox; }

  virtual void set_value(const std::string &value, bool is_null)
  {
    _tbox->set_value(value);
  }
};


class BlobFieldView : public ResultFormView::FieldView
{
  mforms::Box _box;
  mforms::Label _blob;

  void changed()
  {
  }

public:
  BlobFieldView(const std::string &name, bool editable, const boost::function<void (const std::string &s)> &change_callback,
                const boost::function<void ()> &view_callback)
  : FieldView(name, change_callback), _box(true), _blob("BLOB")
  {
    _box.set_spacing(8);
    _box.add(&_blob, false, true);
    mforms::Button *b = mforms::manage(new mforms::Button());
    b->enable_internal_padding(false);
    b->signal_clicked()->connect(view_callback);
    b->set_text("View...");
    _box.add(b, false, true);
  }

  virtual mforms::View *value() { return &_box; }

  virtual void set_value(const std::string &value, bool is_null)
  {
    _blob.set_text(is_null ? "NULL" : "BLOB");
  }
};


static std::list<std::string> parse_enum_definition(const std::string &full_type)
{
  std::list<std::string> l;
  std::string::size_type b, e;

  b = full_type.find('(');
  e = full_type.rfind(')');
  if (b != std::string::npos && e != std::string::npos && e > b)
  {
    bec::tokenize_string_list(full_type.substr(b+1, e-b-1), '\'', true, l);
    for (std::list<std::string>::iterator i = l.begin(); i != l.end(); ++i)
    {
      // strip quotes
      *i = i->substr(1, i->size()-2);
    }
  }
  return l;
}


inline std::string format_label(const std::string &label)
{
  std::string flabel = label + ":";

  if (g_ascii_isalpha(flabel[0]))
    flabel = g_ascii_toupper(flabel[0]) + flabel.substr(1);

  return flabel;
}


ResultFormView::FieldView *ResultFormView::FieldView::create(const Recordset_cdbc_storage::FieldInfo &field,
                                                             const std::string &full_type, bool editable,
                                                             const boost::function<void (const std::string &s)> &callback,
                                                             const boost::function<void ()> &view_blob_callback)
{
  if (field.type == "VARCHAR")
  {
    if (field.display_size > 40)
    {
      TextFieldView *text = new TextFieldView(format_label(field.field), editable, callback);
      if (field.display_size > 1000)
        text->value()->set_size(-1, 200);
      return text;
    }
    else
      return new StringFieldView(format_label(field.field), field.display_size, editable, callback);
  }
  else if (field.type == "TEXT")
  {
    return new TextFieldView(format_label(field.field), editable, callback);
  }
  else if (field.type == "BLOB")
  {
    return new BlobFieldView(format_label(field.field), editable, callback, view_blob_callback);
  }
  else if (field.type == "ENUM" && !full_type.empty())
  {
    return new SelectorFieldView(format_label(field.field), parse_enum_definition(full_type), editable, callback);
  }
  else if (field.type == "SET" && !full_type.empty())
  {
    return new SetFieldView(format_label(field.field), parse_enum_definition(full_type), editable, callback);
  }
  else
    return new StringFieldView(format_label(field.field), field.display_size, editable, callback);
  return NULL;
}


//----------------------------------------------------------------------------------------------------------------


void ResultFormView::navigate(mforms::ToolBarItem *item)
{
  std::string name = item->get_name();
  Recordset::Ref rset(_rset.lock());
  if (rset)
  {
    int row = rset->edited_field_row();
    if (row < 0)
      return;

    if (name == "delete")
    {
      rset->delete_node(row);
    }
    else if (name == "back")
    {
      row--;
      if (row < 0)
        row = 0;
      rset->set_edited_field(row, rset->edited_field_column());
      if (rset->update_edited_field)
        rset->update_edited_field();
    }
    else if (name == "first")
    {
      row = 0;
      rset->set_edited_field(row, rset->edited_field_column());
      if (rset->update_edited_field)
        rset->update_edited_field();
    }
    else if (name == "next")
    {
      row++;
      if (row >= rset->count())
        row = rset->count()-1;
      rset->set_edited_field(row, rset->edited_field_column());
      if (rset->update_edited_field)
        rset->update_edited_field();
    }
    else if (name == "last")
    {
      row = rset->count()-1;
      rset->set_edited_field(row, rset->edited_field_column());
      if (rset->update_edited_field)
        rset->update_edited_field();
    }
    display_record();
  }
}


void ResultFormView::update_value(int column, const std::string &value)
{
  Recordset::Ref rset(_rset.lock());
  if (rset)
  {
    int row = rset->edited_field_row();
    if (rset->count() > row && row >= 0)
      rset->set_field(row, column, value);
  }
}


void ResultFormView::open_field_editor(int column)
{
  Recordset::Ref rset(_rset.lock());
  if (rset)
  {
    int row = rset->edited_field_row();
    if (row < rset->count() && row >= 0)
      rset->open_field_data_editor(row, column);
  }
}

ResultFormView::ResultFormView(bool editable)
: mforms::AppView(false, "ResultFormView", false), _spanel(mforms::ScrollPanelDrawBackground), _tbar(mforms::SecondaryToolBar),
_editable(editable)
{
  mforms::ToolBarItem *item;
  mforms::App *app = mforms::App::get();

  item = mforms::manage(new mforms::ToolBarItem(mforms::TitleItem));
  item->set_text("Form Editor");
  _tbar.add_item(item);
  _tbar.add_separator_item();

  item = mforms::manage(new mforms::ToolBarItem(mforms::LabelItem));
  item->set_text("Navigate:");
  _tbar.add_item(item);

  item = mforms::manage(new mforms::ToolBarItem(mforms::ActionItem));
  item->set_name("first");
  item->set_tooltip("Go to the first row in the recordset.");
  item->signal_activated()->connect(boost::bind(&ResultFormView::navigate, this, item));
  item->set_icon(app->get_resource_path("record_first.png"));
  _tbar.add_item(item);

  item = mforms::manage(new mforms::ToolBarItem(mforms::ActionItem));
  item->set_name("back");
  item->set_tooltip("Go back one row in the recordset.");
  item->signal_activated()->connect(boost::bind(&ResultFormView::navigate, this, item));
  item->set_icon(app->get_resource_path("record_back.png"));
  _tbar.add_item(item);

  _label_item = mforms::manage(new mforms::ToolBarItem(mforms::LabelItem));
  _label_item->set_name("location");
  _tbar.add_item(_label_item);

  item = mforms::manage(new mforms::ToolBarItem(mforms::ActionItem));
  item->set_name("next");
  item->set_tooltip("Go next one row in the recordset.");
  item->signal_activated()->connect(boost::bind(&ResultFormView::navigate, this, item));
  item->set_icon(app->get_resource_path("record_next.png"));
  _tbar.add_item(item);

  item = mforms::manage(new mforms::ToolBarItem(mforms::ActionItem));
  item->set_name("last");
  item->set_tooltip("Go to the last row in the recordset.");
  item->signal_activated()->connect(boost::bind(&ResultFormView::navigate, this, item));
  item->set_icon(app->get_resource_path("record_last.png"));
  _tbar.add_item(item);

  if (editable)
  {
    _tbar.add_separator_item();

    item = mforms::manage(new mforms::ToolBarItem(mforms::LabelItem));
    item->set_text("Edit:");
    _tbar.add_item(item);

    item = mforms::manage(new mforms::ToolBarItem(mforms::ActionItem));
    item->set_name("delete");
    item->set_tooltip("Delete current row from the recordset.");
    item->signal_activated()->connect(boost::bind(&ResultFormView::navigate, this, item));
    item->set_icon(app->get_resource_path("record_del.png"));
    _tbar.add_item(item);

    item = mforms::manage(new mforms::ToolBarItem(mforms::ActionItem));
    item->set_name("last");
    item->set_tooltip("Add a new row to the recordset.");
    item->signal_activated()->connect(boost::bind(&ResultFormView::navigate, this, item));
    item->set_icon(app->get_resource_path("record_add.png"));
    _tbar.add_item(item);
  }

  add(&_tbar, false, true);
  _spanel.set_back_color(mforms::App::get()->get_system_color(mforms::SystemColorContainer).to_html());

  add(&_spanel, true, true);
  _spanel.add(&_table);
  _table.set_column_count(2);
  _table.set_padding(12, 12, 12, 12);
  _table.set_row_spacing(8);
  _table.set_column_spacing(8);
}

ResultFormView::~ResultFormView()
{
  _refresh_ui_connection.disconnect();
  for (std::vector<FieldView*>::const_iterator i = _fields.begin(); i != _fields.end(); ++i)
    delete *i;
}

int ResultFormView::display_record()
{
  Recordset::Ref rset(_rset.lock());
  if (rset)
  {
    int c = 0;

    for (std::vector<FieldView*>::const_iterator i = _fields.begin(); i != _fields.end(); ++i, ++c)
    {
      std::string value;
      rset->get_field_repr_no_truncate(rset->edited_field_row(), c, value);
      (*i)->set_value(value, rset->is_field_null(rset->edited_field_row(), c));
    }

    _label_item->set_text(base::strfmt("%i / %i", rset->edited_field_row()+1, rset->count()));
    _tbar.find_item("first")->set_enabled(rset->edited_field_row() > 0);
    _tbar.find_item("back")->set_enabled(rset->edited_field_row() > 0);

    _tbar.find_item("next")->set_enabled(rset->edited_field_row() < rset->count()-1);
    _tbar.find_item("last")->set_enabled(rset->edited_field_row() < rset->count()-1);
  }
  return 0;
}

std::string ResultFormView::get_full_column_type(SqlEditorForm *editor, const std::string &schema, const std::string &table, const std::string &column)
{
  // we only support 5.5+ for this feature
  if (bec::is_supported_mysql_version_at_least(editor->rdbms_version(), 5, 5))
  {
    std::string q = base::sqlstring("SELECT COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS WHERE table_schema = ? and table_name = ? and column_name = ?", 0) << schema << table << column;
    try
    {
      // XXX handle case where column is an alias, in that case we have to parse the query and extract the original column name by hand
      sql::Dbc_connection_handler::Ref conn;
      base::RecMutexLock lock(editor->ensure_valid_aux_connection(conn));

      std::auto_ptr<sql::Statement> stmt(conn->ref->createStatement());
      std::auto_ptr<sql::ResultSet> result(stmt->executeQuery(q));
      if (result.get() && result->first())
        return result->getString(1);
    }
    catch (std::exception &e)
    {
      log_exception(("Exception getting column information: "+q).c_str(), e);
    }
  }
  return "";
}

void ResultFormView::init_for_resultset(Recordset::Ptr rset_ptr, SqlEditorForm *editor)
{
  Recordset::Ref rset(rset_ptr.lock());
  _rset = rset_ptr;
  if (rset)
  {
    _refresh_ui_connection.disconnect();
    rset->refresh_ui_signal.connect(boost::bind(&ResultFormView::display_record, this));

    int cols = rset->get_column_count();
    _table.set_row_count(cols);

    if (rset->edited_field_row() < 0 && rset->count() > 0)
    {
      rset->set_edited_field(0, 0);
      if (rset->update_edited_field)
        rset->update_edited_field();
    }

    Recordset_cdbc_storage::Ref storage(boost::dynamic_pointer_cast<Recordset_cdbc_storage>(rset->data_storage()));

    std::vector<Recordset_cdbc_storage::FieldInfo> &field_info(storage->field_info());

    int i = 0;
    for (std::vector<Recordset_cdbc_storage::FieldInfo>::const_iterator iter = field_info.begin();
         iter != field_info.end(); ++iter, ++i)
    {
      std::string full_type;

      if ((iter->type == "ENUM" || iter->type == "SET") && !iter->table.empty())
      {
        full_type = get_full_column_type(editor, iter->schema, iter->table, iter->field);
      }

      FieldView *fview = FieldView::create(*iter, full_type, _editable,
                                           boost::bind(&ResultFormView::update_value, this, i, _1),
                                           boost::bind(&ResultFormView::open_field_editor, this, i));
      if (fview)
      {
        _table.add(fview->label(), 0, 1, i, i+1, mforms::HFillFlag);
        _table.add(fview->value(), 1, 2, i, i+1, mforms::HFillFlag | (fview->expands() ? mforms::HExpandFlag : 0));
        _fields.push_back(fview);
      }
    }
  }
}
