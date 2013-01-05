/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  ---
 *
 *  The Verilog frontend.
 *
 *  This frontend is using the AST frontend library (see frontends/ast/).
 *  Thus this frontend does not generate RTLIL code directly but creates an
 *  AST directly from the Verilog parse tree and then passes this AST to
 *  the AST frontend library.
 *
 */

#include "verilog_frontend.h"
#include "kernel/register.h"
#include "kernel/log.h"
#include "kernel/sha1.h"
#include <sstream>
#include <stdarg.h>
#include <assert.h>

using namespace VERILOG_FRONTEND;

// use the Verilog bison/flex parser to generate an AST and use AST::process() to convert it to RTLIL

struct VerilogFrontend : public Frontend {
	VerilogFrontend() : Frontend("verilog") { }
	virtual void execute(FILE *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design)
	{
		bool flag_dump_ast = false;
		bool flag_dump_ast_diff = false;
		bool flag_dump_vlog = false;
		bool flag_nolatches = false;
		bool flag_nomem2reg = false;
		bool flag_ppdump = false;
		bool flag_nopp = false;
		frontend_verilog_yydebug = false;

		log_header("Executing Verilog-2005 frontend.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			std::string arg = args[argidx];
			if (arg == "-dump_ast") {
				flag_dump_ast = true;
				continue;
			}
			if (arg == "-dump_ast_diff") {
				flag_dump_ast = true;
				flag_dump_ast_diff = true;
				continue;
			}
			if (arg == "-dump_vlog") {
				flag_dump_vlog = true;
				continue;
			}
			if (arg == "-yydebug") {
				frontend_verilog_yydebug = true;
				continue;
			}
			if (arg == "-nolatches") {
				flag_nolatches = true;
				continue;
			}
			if (arg == "-nomem2reg") {
				flag_nomem2reg = true;
				continue;
			}
			if (arg == "-ppdump") {
				flag_ppdump = true;
				continue;
			}
			if (arg == "-nopp") {
				flag_nopp = true;
				continue;
			}
			break;
		}
		extra_args(f, filename, args, argidx);

		log("Parsing Verilog input from `%s' to AST representation.\n", filename.c_str());

		AST::current_filename = filename;
		AST::set_line_num = &frontend_verilog_yyset_lineno;
		AST::get_line_num = &frontend_verilog_yyget_lineno;

		current_ast = new AST::AstNode(AST::AST_DESIGN);

		FILE *fp = f;
		std::string code_after_preproc;

		if (!flag_nopp) {
			code_after_preproc = frontend_verilog_preproc(f, filename);
			if (flag_ppdump)
				log("-- Verilog code after preprocessor --\n%s-- END OF DUMP --\n", code_after_preproc.c_str());
			fp = fmemopen((void*)code_after_preproc.c_str(), code_after_preproc.size(), "r");
		}

		lexer_feature_defattr = false;

		frontend_verilog_yyset_lineno(1);
		frontend_verilog_yyrestart(fp);
		frontend_verilog_yyparse();
		frontend_verilog_yylex_destroy();

		AST::process(design, current_ast, flag_dump_ast, flag_dump_ast_diff, flag_dump_vlog, flag_nolatches, flag_nomem2reg);

		if (!flag_nopp)
			fclose(fp);

		delete current_ast;
		current_ast = NULL;

		log("Successfully finished Verilog frontend.\n");
	}
} VerilogFrontend;

// the yyerror function used by bison to report parser errors
void frontend_verilog_yyerror(char const *fmt, ...)
{
	va_list ap;
	char buffer[1024];
	char *p = buffer;
	p += snprintf(p, buffer + sizeof(buffer) - p, "Parser error in line %s:%d: ",
			AST::current_filename.c_str(), frontend_verilog_yyget_lineno());
	va_start(ap, fmt);
	p += vsnprintf(p, buffer + sizeof(buffer) - p, fmt, ap);
	va_end(ap);
	p += snprintf(p, buffer + sizeof(buffer) - p, "\n");
	log_error("%s", buffer);
	exit(1);
}

