/*
   Source for blc IdaPro plugin
   Copyright (c) 2019 Chris Eagle

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2 of the License, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
   more details.

   You should have received a copy of the GNU General Public License along with
   this program; if not, write to the Free Software Foundation, Inc., 59 Temple
   Place, Suite 330, Boston, MA 02111-1307 USA
*/

#ifndef USE_DANGEROUS_FUNCTIONS
#define USE_DANGEROUS_FUNCTIONS 1
#endif  // USE_DANGEROUS_FUNCTIONS

#ifndef USE_STANDARD_FILE_FUNCTIONS
#define USE_STANDARD_FILE_FUNCTIONS
#endif

#ifndef NO_OBSOLETE_FUNCS
#define NO_OBSOLETE_FUNCS
#endif

#include <pro.h>
#include <ida.hpp>
#include <idp.hpp>
#include <bytes.hpp>
#include <loader.hpp>
#include <kernwin.hpp>
#include <netnode.hpp>
#include <typeinf.hpp>
#include <struct.hpp>
#include <range.hpp>
#include <frame.hpp>
#include <segment.hpp>
#include <funcs.hpp>
#include <search.hpp>
#include <diskio.hpp>
#include <segregs.hpp>
#include <xref.hpp>
#include <help.h>

#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <map>
#include <set>

#include "plugin.hh"
#include "ast.hh"

#if defined(__NT__)                   // MS Windows
#define DIRSEP "\\"
#else
#define DIRSEP "/"
#endif

using std::iostream;
using std::ifstream;
using std::istreambuf_iterator;
using std::map;
using std::set;

struct LocalVar {
   string ghidra_name;
   string current_name;  //current display name in disassembly display
   ea_t offset;      //offset into stack frame if stack var (BADADDR otherwise)

   LocalVar(const string &gname, const string &iname, ea_t _offset = BADADDR) :
      ghidra_name(gname), current_name(iname), offset(_offset) {};
};

struct Decompiled {
   Function *ast;
   func_t *ida_func;
   map<string, LocalVar*> locals;

   Decompiled(Function *f, func_t *func) : ast(f), ida_func(func) {};
   ~Decompiled();
};

Decompiled::~Decompiled() {
   delete ast;
   for (map<string, LocalVar*>::iterator i = locals.begin(); i != locals.end(); i++) {
      delete i->second;
   }
}

void decompile_at(ea_t ea, TWidget *w = NULL);
int do_ida_rename(qstring &name, ea_t func);

static map<string,uint32_t> type_sizes;
static map<TWidget*,qvector<ea_t> > histories;
static map<TWidget*,string> views;
static map<TWidget*,Decompiled*> function_map;
static set<string> titles;

arch_map_t arch_map;

static string get_available_title() {
   string title("A");
   while (titles.find(title) != titles.end()) {
      int i = 0;
      while (true) {
         title[i] += 1;
         if (title[i] > 'Z') {
            title[i] = 'A';
            if (title.length() == i) {
               title.push_back('A');
               break;
            }
            else {
               i++;
            }
         }
         else {
            break;
         }
      }
   }
   return title;
}

//---------------------------------------------------------------------------
// get the word under the (keyboard or mouse) cursor
static bool get_current_word(TWidget *v, bool mouse, qstring &word) {
   // query the cursor position
   int x, y;
   if (get_custom_viewer_place(v, mouse, &x, &y) == NULL) {
      return false;
   }
   // query the line at the cursor
   qstring buf;
   tag_remove(&buf, get_custom_viewer_curline(v, mouse));
   if (x >= buf.length()) {
      return false;
   }
   char *ptr = buf.begin() + x;
   char *end = ptr;
   // find the end of the word
   while ((qisalnum(*end) || *end == '_') && *end != '\0') {
      end++;
   }

   if (end == ptr) {
      return false;
   }

   // find the beginning of the word
   while (ptr > buf.begin() && (qisalnum(ptr[-1]) || ptr[-1] == '_')) {
      ptr--;
   }
   word = qstring(ptr, end - ptr);
   return true;
}

static bool navigate_to_word(TWidget *w, bool cursor) {
   qstring word;
   if (get_current_word(w, cursor, word)) {
      ea_t ea = get_name_ea(BADADDR, word.c_str());
      if (ea != BADADDR) {
         if (is_function_start(ea) && !is_extern_addr(ea)) {
            map<TWidget*,qvector<ea_t> >::iterator mi = histories.find(w);
            if (mi == histories.end() || mi->second.size() == 0 || mi->second.back() != ea) {
               histories[w].push_back(ea);
               decompile_at(ea, w);
            }
         }
         else {
            jumpto(ea);
         }
         return true;
      }
   }
   return false;
}

//---------------------------------------------------------------------------
// Keyboard callback
static bool idaapi ct_keyboard(TWidget *w, int key, int shift, void *ud) {
   ea_t addr = 0;
   if (shift == 0) {
      strvec_t *sv = (strvec_t *)ud;
      switch (key) {
         case 'G':
            if (ask_addr(&addr, "Jump address")) {
               func_t *f = get_func(addr);
               if (f) {
                  decompile_at(f->start_ea, w);
               }
            }
            return true;
         case 'N': { //rename the thing under the cursor
            Decompiled *dec = function_map[w];
            qstring word;
            bool refresh = false;
            if (get_current_word(w, false, word)) {
               string sword(word.c_str());
//               msg("Try to rename: %s\n", word.c_str());
               if (!is_reserved(sword)) {
                  qstring new_name(word);
                  map<string,LocalVar*>::iterator mi = dec->locals.find(sword);
                  if (mi != dec->locals.end()) {
//                     msg("%s is a local\n", word.c_str());
                     LocalVar *lv = mi->second;
                     if (ask_str(&word, HIST_IDENT, "Please enter item name") && sword != word.c_str()) {
                        string newname(word.c_str());
                        //need to make sure new name will be legal
                        if (is_reserved(newname) || dec->locals.find(newname) != dec->locals.end() ||
                            get_name_ea(BADADDR, newname.c_str()) != BADADDR) {
//                           msg("rename fail 1\n");
                           return true;
                        }
                        if (lv->offset != BADADDR) { //stack var
//                           msg("renaming a stack var %s to %s\n", sword.c_str(), word.c_str());
                           if (set_member_name(get_frame(dec->ida_func), lv->offset, word.c_str())) {
                              lv->current_name = newname;
                              dec->locals.erase(sword);
                              dec->locals[newname] = lv;
                              dec->ast->rename(sword, newname);
                              refresh = true;
                           }
                           else {
//                              msg("set_member_name failed\n");
                           }
                        }
                        else { //not stack var, reg var??
                           qstring iname;
                           netnode nn(dec->ida_func->start_ea);
//                           msg("renaming a reg var %s to %s\n", sword.c_str(), word.c_str());
                           lv->current_name = newname;
                           dec->locals.erase(sword);
                           dec->locals[word.c_str()] = lv;
                           dec->ast->rename(sword, word.c_str());
                           nn.hashset(lv->ghidra_name.c_str(), word.c_str());
                           refresh = true;
                        }
                     }
                  }
                  else if (do_ida_rename(new_name, dec->ida_func->start_ea) == 2) {
                     string snew_name(new_name.c_str());
                     dec->ast->rename(sword, snew_name);
//                     msg("rename: %s -> %s\n", word.c_str(), new_name.c_str());
                     refresh = true;
                  }
                  else {
                  }
               }
               else {
                  //if the user entered a type name we need to do something different
               }
            }
            if (refresh) {
               vector<string> code;
               dec->ast->print(&code);
               strvec_t *sv = new strvec_t();
               for (vector<string>::iterator si = code.begin(); si != code.end(); si++) {
                  sv->push_back(simpleline_t(si->c_str()));
               }

               sv = (strvec_t*)callui(ui_custom_viewer_set_userdata, w, sv).vptr;
               refresh_custom_viewer(w);
               repaint_custom_viewer(w);
               delete sv;
            }
            return true;
         }
         case 'Y': { //Set type for the thing under the cursor
            qstring word;
            int x, y;
            if (get_custom_viewer_place(w, false, &x, &y) == NULL) {
               return false;
            }
            if (get_current_word(w, false, word)) {
//               msg("(%d)type: %s\n", y, word.c_str());
            }
            return true;
         }
         case IK_DIVIDE: { //Add eol comment on current line
            int x, y;
            if (get_custom_viewer_place(w, false, &x, &y) == NULL) {
               return false;
            }
//            msg("add comment on line %d\n", y);
            return true;
         }
         case IK_ESCAPE: {
            map<TWidget*,qvector<ea_t> >::iterator mi = histories.find(w);
            if (mi != histories.end()) {
               qvector<ea_t> &v = mi->second;
               if (v.size() == 1) {
                  close_widget(w, WCLS_DONT_SAVE_SIZE | WCLS_CLOSE_LATER);
                  string t = views[w];
                  views.erase(w);
                  delete function_map[w];
                  function_map.erase(w);
                  titles.erase(t);
               }
               else {
                  v.pop_back();
                  decompile_at(v.back(), w);
               }
               return true;
            }
            break;
         }
         case IK_RETURN: {  //jump to symbol under cursor
            return navigate_to_word(w, false);
         }
         default:
//            msg("Detected key press: 0x%x\n", key);
            break;
       }
   }
   return false;
}

static bool idaapi ct_dblclick(TWidget *cv, int shift, void *ud) {
//   msg("Double clicked on: %s\n", word.c_str());
   return navigate_to_word(cv, true);
}

static const custom_viewer_handlers_t handlers(
        ct_keyboard,
        NULL, // popup
        NULL, // mouse_moved
        NULL, // click
        ct_dblclick, // dblclick
        NULL, //ct_curpos,
        NULL, // close
        NULL, // help
        NULL);// adjust_place

string ghidra_dir;

map<int,string> proc_map;

map<int,string> return_reg_map;

int idaapi blc_init(void);

void idaapi blc_term(void);

static const char *name_dialog;

//get the format string for IDA's standard rename dialog
void find_ida_name_dialog() {
   help_t i;
   for (i = 0; ; i++) {
      const char *hlp = itext(i);
      const char *lf = strchr(hlp, '\n');
      if (lf != NULL) {
         lf++;
         if (strncmp("Rename address\n", lf, 15) == 0) {
            name_dialog = hlp;
//            msg("Found:\n%s\n", hlp);
            break;
         }
      }
   }
}

// return -1 - name is not associated with a symbol
// return 0  - duplicate name
// return 1  - no change
// return 2  - name changed
// return 3  - new name, but couldn't change it
int do_ida_rename(qstring &name, ea_t func) {
   ea_t name_ea = get_name_ea(func, name.c_str());
   if (name_ea == BADADDR) {
      //somehow the original name is invalid
//      msg("rename: %s has no addr\n", name.c_str());
      return -1;
   }
   qstring orig = name;
   bool res = ask_str(&name, HIST_IDENT, "Please enter item name");
   if (res && name != orig) {
      ea_t new_name_ea = get_name_ea(func, name.c_str());
      if (new_name_ea != BADADDR) {
         //new name is same as existing name
//         msg("rename: new name already in use\n", name.c_str());
         return 0;
      }
//      msg("Custom rename: %s at adddress 0x%zx\n", name.c_str(), name_ea);
      res = set_name(name_ea, name.c_str());
      return res ? 2 : 3;
   }
//   msg("rename: no change\n");
   return 1;
}

void init_ida_ghidra() {
   const char *ghidra = getenv("GHIDRA_DIR");
   if (ghidra) {
      ghidra_dir = ghidra;
   }
   else {
      ghidra_dir = idadir("plugins");
   }
//   find_ida_name_dialog();

   arch_map[PLFM_MIPS] = mips_setup;

   proc_map[PLFM_6502] = "6502";
   proc_map[PLFM_68K] = "68000";
   proc_map[PLFM_6800] = "6805";
   //proc_map[PLFM_xxx] = "8048";
   proc_map[PLFM_8051] = "8051";
   //proc_map[PLFM_Z80] = "8085";
   proc_map[PLFM_ARM] = "ARM";
   //proc_map[PLFM_ARM] = "AARCH64";
   proc_map[PLFM_AVR] = "Atmel";
   proc_map[PLFM_CR16] = "CR16";
   proc_map[PLFM_DALVIK] = "Dalvik";
   proc_map[PLFM_JAVA] = "JVM";
   proc_map[PLFM_MIPS] = "MIPS";
   proc_map[PLFM_HPPA] = "PA-RISC";
   proc_map[PLFM_PIC] = "PIC";
   proc_map[PLFM_PPC] = "PowerPC";
   proc_map[PLFM_SPARC] = "Sparc";
   proc_map[PLFM_MSP430] = "TI_MSP430";
   proc_map[PLFM_TRICORE] = "tricore";
   proc_map[PLFM_386] = "x86";
   proc_map[PLFM_Z80] = "Z80";

   return_reg_map[PLFM_6502] = "6502";
   return_reg_map[PLFM_68K] = "68000";
   return_reg_map[PLFM_6800] = "6805";
   //return_reg_map[PLFM_xxx] = "8048";
   return_reg_map[PLFM_8051] = "8051";
   //return_reg_map[PLFM_Z80] = "8085";
   return_reg_map[PLFM_ARM] = "r0:r0:r0:r0";
   //return_reg_map[PLFM_ARM] = "r0:r0:r0:r0";
   return_reg_map[PLFM_AVR] = "Atmel";
   return_reg_map[PLFM_CR16] = "CR16";
   return_reg_map[PLFM_DALVIK] = "Dalvik";
   return_reg_map[PLFM_JAVA] = "JVM";
   return_reg_map[PLFM_MIPS] = "v0:v0:v0:v0";
   return_reg_map[PLFM_HPPA] = "PA-RISC";
   return_reg_map[PLFM_PIC] = "PIC";
   return_reg_map[PLFM_PPC] = "PowerPC";
   return_reg_map[PLFM_SPARC] = "Sparc";
   return_reg_map[PLFM_MSP430] = "TI_MSP430";
   return_reg_map[PLFM_TRICORE] = "tricore";
   return_reg_map[PLFM_386] = "al:ax:eax:rax";
   return_reg_map[PLFM_Z80] = "Z80";

   type_sizes["void"] = 1;
   type_sizes["bool"] = 1;
   type_sizes["uint1"] = 1;
   type_sizes["uint2"] = 2;
   type_sizes["uint4"] = 4;
   type_sizes["uint8"] = 8;
   type_sizes["int1"] = 1;
   type_sizes["int2"] = 2;
   type_sizes["int4"] = 4;
   type_sizes["int8"] = 8;
   type_sizes["float4"] = 4;
   type_sizes["float8"] = 8;
   type_sizes["float10"] = 10;
   type_sizes["float16"] = 16;
   type_sizes["xunknown1"] = 1;
   type_sizes["xunknown2"] = 2;
   type_sizes["xunknown4"] = 4;
   type_sizes["xunknown8"] = 8;
   type_sizes["code"] = 1;
   type_sizes["char"] = 1;
   type_sizes["wchar2"] = 2;
   type_sizes["wchar4"] = 4;
}

#if IDA_SDK_VERSION < 730

#define WOPN_DP_TAB WOPN_TAB

bool inf_is_64bit() {
   return inf.is_64bit();
}

bool inf_is_32bit() {
   return inf.is_32bit();
}

void inf_get_cc(compiler_info_t *cc) {
   *cc = inf.cc;
}

bool inf_is_be() {
   return inf.is_be();
}

filetype_t inf_get_filetype() {
   return (filetype_t)inf.filetype;
}

#endif

int get_proc_id() {
   return ph.id;
}

bool get_sleigh_id(string &sleigh) {
   sleigh.clear();
   map<int,string>::iterator proc = proc_map.find(ph.id);
   if (proc == proc_map.end()) {
      return false;
   }
   compiler_info_t cc;
   inf_get_cc(&cc);
   bool is_64 = inf_is_64bit();
   bool is_be = inf_is_be();
   filetype_t ftype = inf_get_filetype();

   sleigh = proc->second + (is_be ? ":BE" : ":LE");

   switch (ph.id) {
      case PLFM_6502:
         sleigh += ":16:default";
         break;
      case PLFM_68K:
         //options include "default" "MC68030" "MC68020" "Coldfire"
         sleigh += ":32:default";
         break;
      case PLFM_6800:
         sleigh += ":8:default";
         break;
      case PLFM_8051:
         sleigh += ":16:default";
         break;
      case PLFM_ARM:
         //options include "v8" "v8T" "v8LEInstruction" "v7" "v7LEInstruction" "Cortex"
         //                "v6" "v5t" "v5" "v4t" "v4" "default"
         if (is_64) {  //AARCH64
            sleigh = "AARCH64";
            sleigh += (is_be ? ":BE:64:v8A" : ":LE:64:v8A");
         }
         else {
            sleigh += ":32:v7";
         }
         break;
      case PLFM_AVR:
         sleigh += ":16:default";
         break;
      case PLFM_CR16:
         sleigh += ":16:default";
         break;
      case PLFM_DALVIK:
         break;
      case PLFM_JAVA:
         break;
      case PLFM_MIPS: {
         //options include "R6" "micro" "64-32addr" "micro64-32addr" "64-32R6addr" "default"
         qstring abi;
         if (get_abi_name(&abi) > 0 && abi.find("n32") == 0) {
            sleigh += ":64:64-32addr";
         }
         else {
            sleigh += is_64 ? ":64:default" : ":32:default";
         }
         break;
      }
      case PLFM_HPPA:
         break;
      case PLFM_PIC:
         break;
      case PLFM_PPC: {
         //options include "default" "64-32addr" "4xx" "MPC8270" "QUICC" "A2-32addr"
         //                "A2ALT-32addr" "A2ALT" "VLE-32addr" "VLEALT-32addr"
         qstring abi;
         if (get_abi_name(&abi) > 0 && abi.find("xbox") == 0) {
            // ABI name is set to "xbox" for X360 PPC executables
            sleigh += ":64:VLE-32addr";
         }
         else {
            sleigh += is_64 ? ":64:default" : ":32:default";
         }
         break;
      }
      case PLFM_SPARC:
         break;
      case PLFM_MSP430:
         break;
      case PLFM_TRICORE:
         break;
      case PLFM_386:
         //options include "System Management Mode" "Real Mode" "Protected Mode" "default"
         sleigh += is_64 ? ":64" : (inf_is_32bit() ? ":32" : ":16");
         sleigh += ":default";

         if (cc.id == COMP_BC) {
            sleigh += ":borlandcpp";
         }
         else if (cc.id == COMP_MS) {
            sleigh += ":windows";
         }
         else if (cc.id == COMP_GNU) {
            sleigh += ":gcc";
         }
         break;
      case PLFM_Z80:
         break;
      default:
         return false;
   }
   msg("Using sleigh id: %s\n", sleigh.c_str());
   return true;
}

void get_ida_bytes(uint8_t *buf, uint64_t size, uint64_t ea) {
   get_bytes(buf, size, (ea_t)ea);
}

bool does_func_return(void *func) {
   func_t *f = (func_t*)func;
   return func_does_return(f->start_ea);
}

uint64_t get_func_start(void *func) {
   func_t *f = (func_t*)func;
   return f->start_ea;
}

uint64_t get_func_start(uint64_t ea) {
   func_t *f = get_func((ea_t)ea);
   return f ? f->start_ea : BADADDR;
}

uint64_t get_func_end(uint64_t ea) {
   func_t *f = get_func((ea_t)ea);
   return f ? f->end_ea : BADADDR;
}

//Create a Ghidra to Ida name mapping for a single loval variable (including formal parameters)
void map_var_from_decl(Decompiled *dec, VarDecl *decl) {
   Function *ast = dec->ast;
   func_t *func = dec->ida_func;
   struc_t *frame = get_frame(func);
   ea_t ra = frame_off_retaddr(func);
   const string gname = decl->getName();
   size_t stack = gname.find("Stack");
   LocalVar *lv = new LocalVar(gname, gname);  //default current name will be ghidra name
   if (stack != string::npos) {         //if it's a stack var, change current to ida name
      uint32_t stackoff = strtoul(&gname[stack + 5], NULL, 0);
      member_t *var = get_member(frame, ra - stackoff);
      lv->offset = ra - stackoff;
      if (var) {                        //now we know there's an ida name assigned
         qstring iname;
         get_member_name(&iname, var->id);
         ast->rename(gname, iname.c_str());
         dec->locals[iname.c_str()] = lv;
         lv->current_name = iname.c_str();
      }
      else {  //ghidra says there's a variable here, let's name it in ida
         //TODO - need to compute sizeof(decl) to properly create
         //       the new data member
         qstring iname;
         iname.sprnt("var_%X", stackoff - func->frregs);
         if (add_struc_member(frame, iname.c_str(), ra - stackoff, byte_flag(), NULL, 1) == 0) {
            ast->rename(gname, iname.c_str());
            dec->locals[iname.c_str()] = lv;
            lv->current_name = iname.c_str();
         }
         else {
            dec->locals[gname] = lv;
         }
      }
   }
   else {  //handle non-stack (register) local variables
      netnode nn(dec->ida_func->start_ea);
      qstring iname;
      if (nn.hashstr(&iname, gname.c_str()) <= 0) {
         //no existing mapping
         dec->locals[gname] = lv;
      }
      else {
         //we already have a mapping for this ghidra variable
         ast->rename(gname, iname.c_str());
         dec->locals[iname.c_str()] = lv;
         lv->current_name = iname.c_str();
      }
   }
}

void map_ghidra_to_ida(Decompiled *dec) {
   Function *ast = dec->ast;
   vector<Statement*> &bk = ast->block.block;
   vector<VarDecl*> &parms = ast->prototype.parameters;

   //add mappings for formal parameter names
   for (vector<VarDecl*>::iterator i = parms.begin(); i != parms.end(); i++) {
      VarDecl *decl = *i;
      map_var_from_decl(dec, decl);
   }

   //add mappings for variable names
   for (vector<Statement*>::iterator i = bk.begin(); i != bk.end(); i++) {
      VarDecl *decl = dynamic_cast<VarDecl*>(*i);
      if (decl) {
         map_var_from_decl(dec, decl);
      }
      else {
         break;
      }
   }
}

void decompile_at(ea_t addr, TWidget *w) {
   string xml;
   string cfunc;
   func_t *func = get_func(addr);
   Function *ast = NULL;
   if (func) {
      int res = do_decompile(func->start_ea, func->end_ea, &ast);
      if (ast) {
//         msg("got a Functon tree!\n");
         Decompiled *dec = new Decompiled(ast, func);

         //now try to map ghidra stack variable names to ida stack variable names
         map_ghidra_to_ida(dec);

         vector<string> code;
         dec->ast->print(&code);
         strvec_t *sv = new strvec_t();
         for (vector<string>::iterator si = code.begin(); si != code.end(); si++) {
            sv->push_back(simpleline_t(si->c_str()));
         }

         qstring func_name;
         qstring fmt;
         get_func_name(&func_name, func->start_ea);
         string title = get_available_title();
         fmt.sprnt("Ghidra code  - %s", title.c_str());   // make the suffix change with more windows

         simpleline_place_t s1;
         simpleline_place_t s2((int)(sv->size() - 1));

         if (w == NULL) {
            TWidget *cv = create_custom_viewer(fmt.c_str(), &s1, &s2,
                                               &s1, NULL, sv, &handlers, sv);
            function_map[cv] = dec;
            TWidget *code_view = create_code_viewer(cv);
            set_code_viewer_is_source(code_view);
            display_widget(code_view, WOPN_DP_TAB);
            histories[cv].push_back(addr);
            views[cv] = title;
            titles.insert(title);
         }
         else {
            sv = (strvec_t*)callui(ui_custom_viewer_set_userdata, w, sv).vptr;
            refresh_custom_viewer(w);
            repaint_custom_viewer(w);
            delete function_map[w];
            function_map[w] = dec;
            delete sv;
         }
      }
//      msg("do_decompile returned: %d\n%s\n%s\n", res, code.c_str(), cfunc.c_str());
   }
}

const char *tag_remove(const char *tagged) {
   static qstring ll;
   tag_remove(&ll, tagged);
   return ll.c_str();
}

bool idaapi blc_run(size_t /*arg*/) {
   ea_t addr = get_screen_ea();
   decompile_at(addr);
   return true;
}

int64_t get_name(string &name, uint64_t ea, int flags) {
   qstring ida_name;
   int64_t res = get_name(&ida_name, (ea_t)ea, flags);
   if (res > 0) {
      name = ida_name.c_str();
   }
   return res;
}

int64_t get_func_name(string &name, uint64_t ea) {
   qstring ida_name;
   int64_t res = get_func_name(&ida_name, (ea_t)ea);
   if (res > 0) {
      name = ida_name.c_str();
   }
   return res;
}

bool is_function_start(uint64_t ea) {
   func_t *f = get_func((ea_t)ea);
   return f != NULL && f->start_ea == (ea_t)ea;
}

void get_input_file_path(string &path) {
   char buf[512];
   get_input_file_path(buf, sizeof(buf));
   path = buf;
}

bool is_thumb_mode(uint64_t ea) {
   return get_sreg((ea_t)ea, 20) == 1;
}

//is ea a function internal jump target, if so
//return true and place its name in name
//else return false
bool is_code_label(uint64_t ea, string &name) {
   xrefblk_t xr;
   for (bool success = xr.first_to((ea_t)ea, XREF_ALL); success; success = xr.next_to()) {
      if (xr.iscode == 0) {
         break;
      }
      if (xr.type != fl_JN) {
         continue;
      }
      qstring ida_name;
      int64_t res = get_name(&ida_name, (ea_t)ea, GN_LOCAL);
      if (res > 0) {
         name = ida_name.c_str();
         return true;
      }
   }
   return false;
}

bool is_extern_addr(uint64_t ea) {
   qstring sname;
   segment_t *s = getseg(ea);
   if (s) {
      get_segm_name(&sname, s);
      if (sname == "extern") {
         return true;
      }
   }
   return false;
}

bool is_external_ref(uint64_t ea, uint64_t *fptr) {
   ea_t got;
   func_t *pfn = get_func((ea_t)ea);
   if (pfn == NULL) {
      return false;
   }
   if (is_extern_addr(pfn->start_ea)) {
      if (fptr) {
         *fptr = pfn->start_ea;
      }
      return true;
   }
   ea_t _export = calc_thunk_func_target(pfn, &got);
   bool res = _export != BADADDR;
   if (res) {
      if (fptr) {
         *fptr = got;
      }
      msg("0x%zx is external, with got entry at 0x%zx\n", ea, (size_t)got);
   }
   return res;
}

bool is_extern(const string &name) {
   bool res = false;
   ea_t ea = get_name_ea(BADADDR, name.c_str());
   if (ea == BADADDR) {
      return false;
   }
   if (is_function_start(ea)) {
      res = is_external_ref(ea, NULL);
   }
   else {
      res = is_extern_addr(ea);
   }
//   msg("is_extern called for %s (%d)\n", name.c_str(), res);
   return res;
}

bool address_of(const string &name, uint64_t *addr) {
   bool res = false;
   ea_t ea = get_name_ea(BADADDR, name.c_str());
   if (ea == BADADDR) {
      return false;
   }
   *addr = ea;
   return true;
}

bool is_library_func(const string &name) {
   bool res = false;
   ea_t ea = get_name_ea(BADADDR, name.c_str());
   if (is_function_start(ea)) {
      func_t *f = get_func(ea);
      res = f ? (f->flags & FUNC_LIB) != 0 : false;
   }
   return res;
}

bool is_named_addr(uint64_t ea, string &name) {
   qstring res;
   //a sanity check on ea
   segment_t *s = getseg(0);
   if (s != NULL && ea < s->end_ea) {
      //ea falls in first segment of zero based binary
      //this are generally headers and ea is probably
      //not a pointer but instead just a small number
      return false;
   }
   if (get_name(&res, (ea_t)ea) > 0) {
      name = res.c_str();
      return true;
   }
   return false;
}

bool is_pointer_var(uint64_t ea, uint32_t size, uint64_t *tgt) {
   xrefblk_t xb;
   if (xb.first_from(ea, XREF_DATA) && xb.type == dr_O) {
      // xb.to - contains the referenced address
      *tgt = xb.to;
      return true;
   }
   return false;
}

bool is_read_only(uint64_t ea) {
   qstring sname;
   segment_t *s = getseg(ea);
   if (s) {
      if ((s->perm & SEGPERM_WRITE) == 0) {
         return true;
      }
      //not explicitly read only, so let's make some guesses
      //based on the segment name
      get_segm_name(&sname, s);
      if (sname.find("got") <= 1) {
         return true;
      }
      if (sname.find("rodata") <= 1) {
         return true;
      }
      if (sname.find("rdata") <= 1) {
         return true;
      }
      if (sname.find("idata") <= 1) {
         return true;
      }
      if (sname.find("rel.ro") != qstring::npos) {
         return true;
      }
   }
   return false;
}

bool simplify_deref(const string &name, string &new_name) {
   uint64_t tgt;
   ea_t addr = get_name_ea(BADADDR, name.c_str());
   if (addr != BADADDR && is_read_only(addr) && is_pointer_var(addr, (uint32_t)ph.max_ptr_size(), &tgt)) {
      if (get_name(new_name, tgt, 0)) {
//         msg("could simplify *%s to %s\n", name.c_str(), new_name.c_str());
         return true;
      }
   }
   return false;
}

void adjust_thunk_name(string &name) {
   ea_t ea = get_name_ea(BADADDR, name.c_str());
   if (is_function_start(ea)) {
      func_t *f = get_func(ea);
      ea_t fun = calc_thunk_func_target(f, &ea);
      if (fun != BADADDR) {
         qstring tname;
         if (get_name(&tname, fun)) {
            name = tname.c_str();
         }
      }
   }
}

//TODO think about sign extension for values smaller than 8 bytes
bool get_value(uint64_t addr, uint64_t *val) {
   flags_t f = get_full_flags(addr);
   if (is_qword(f)) {
      *val = get_qword(addr);
   }
   else if (is_dword(f)) {
      *val = get_dword(addr);
   }
   else if (is_byte(f)) {
      *val = get_byte(addr);
   }
   else if (is_word(f)) {
      *val = get_word(addr);
   }
   else {
      return false;
   }
   return true;
}

bool get_string(uint64_t addr, string &str) {
   qstring res;
   flags_t f = get_full_flags(addr);
   if (is_strlit(f)) {
      get_strlit_contents(&res, addr, -1, STRTYPE_C);
      str = res.c_str();
      return true;
   }
   else if (!is_data(f)) {
      size_t maxlen = get_max_strlit_length(addr, STRTYPE_C);
      if (maxlen > 4) {
         create_strlit(addr, 0, STRTYPE_C);
         get_strlit_contents(&res, addr, -1, STRTYPE_C);
         str = res.c_str();
         return true;
      }
   }
   return false;
}

//--------------------------------------------------------------------------
char comment[] = "Ghidra decompiler integration.";

char help[] = "I have nothing to offer.\n";

char wanted_name[] = "Ghidra Decompiler";

char wanted_hotkey[] = "Alt-F3";

plugin_t PLUGIN =
{
  IDP_INTERFACE_VERSION,
  0,                    // plugin flags
  blc_init,          // initialize
  blc_term,          // terminate. this pointer may be NULL.
  blc_run,           // invoke plugin
  comment,              // long comment about the plugin
                        // it could appear in the status line
                        // or as a hint
  help,                 // multiline help about the plugin
  wanted_name,          // the preferred short name of the plugin
  wanted_hotkey         // the preferred hotkey to run the plugin
};
