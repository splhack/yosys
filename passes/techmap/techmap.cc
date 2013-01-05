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
 */

#include "kernel/register.h"
#include "kernel/log.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "passes/techmap/stdcells.inc"

static void apply_prefix(std::string prefix, std::string &id)
{
	if (id[0] == '\\')
		id = prefix + "." + id.substr(1);
	else
		id = prefix + "." + id;
}

static void apply_prefix(std::string prefix, RTLIL::SigSpec &sig, RTLIL::Module *module)
{
	for (size_t i = 0; i < sig.chunks.size(); i++) {
		if (sig.chunks[i].wire == NULL)
			continue;
		std::string wire_name = sig.chunks[i].wire->name;
		apply_prefix(prefix, wire_name);
		assert(module->wires.count(wire_name) > 0);
		sig.chunks[i].wire = module->wires[wire_name];
	}
}

std::map<std::pair<RTLIL::IdString, std::map<RTLIL::IdString, RTLIL::Const>>, RTLIL::Module*> techmap_cache;

static bool techmap_module(RTLIL::Module *module, RTLIL::Design *map)
{
	bool did_something = false;

	std::vector<std::string> cell_names;

	for (auto &cell_it : module->cells)
		cell_names.push_back(cell_it.first);

	for (auto &cell_name : cell_names)
	{
		if (module->cells.count(cell_name) == 0)
			continue;

		RTLIL::Cell *cell = module->cells[cell_name];

		if (map->modules.count(cell->type) == 0)
			continue;

		RTLIL::Module *tpl = map->modules[cell->type];
		std::pair<RTLIL::IdString, std::map<RTLIL::IdString, RTLIL::Const>> key(cell->type, cell->parameters);

		if (techmap_cache.count(key) > 0) {
			tpl = techmap_cache[key];
		} else {
			std::string derived_name = cell->type;
			if (cell->parameters.size() != 0) {
				derived_name = tpl->derive(map, cell->parameters);
				tpl = map->modules[derived_name];
				log_header("Continuing TECHMAP pass.\n");
			}
			for (auto &cit : tpl->cells)
				if (cit.second->type == "\\TECHMAP_FAILED") {
					log("Not using module `%s' from techmap as it contains a TECHMAP_FAILED marker cell.\n", derived_name.c_str());
					tpl = NULL;
					break;
				}
			techmap_cache[key] = tpl;
		}

		if (tpl == NULL)
			goto next_cell;

		log("Mapping `%s.%s' using `%s'.\n", module->name.c_str(), cell_name.c_str(), tpl->name.c_str());

		if (tpl->memories.size() != 0)
			log_error("Technology map yielded memories -> this is not supported.");

		if (tpl->processes.size() != 0)
			log_error("Technology map yielded processes -> this is not supported.");

		for (auto &it : tpl->wires) {
			RTLIL::Wire *w = new RTLIL::Wire(*it.second);
			apply_prefix(cell_name, w->name);
			w->port_input = false;
			w->port_output = false;
			w->port_id = 0;
			module->wires[w->name] = w;
		}

		for (auto &it : tpl->cells) {
			RTLIL::Cell *c = new RTLIL::Cell(*it.second);
			if (c->type.substr(0, 2) == "\\$")
				c->type = c->type.substr(1);
			apply_prefix(cell_name, c->name);
			for (auto &it2 : c->connections)
				apply_prefix(cell_name, it2.second, module);
			module->cells[c->name] = c;
		}

		for (auto &it : tpl->connections) {
			RTLIL::SigSig c = it;
			apply_prefix(cell_name, c.first, module);
			apply_prefix(cell_name, c.second, module);
			module->connections.push_back(c);
		}

		for (auto &it : cell->connections) {
			assert(tpl->wires.count(it.first));
			assert(tpl->wires[it.first]->port_id > 0);
			RTLIL::Wire *w = tpl->wires[it.first];
			RTLIL::SigSig c;
			if (w->port_output) {
				c.first = it.second;
				c.second = RTLIL::SigSpec(w);
				apply_prefix(cell_name, c.second, module);
			} else {
				c.first = RTLIL::SigSpec(w);
				c.second = it.second;
				apply_prefix(cell_name, c.first, module);
			}
			if (c.second.width > c.first.width)
				c.second.remove(c.first.width, c.second.width - c.first.width);
			if (c.second.width < c.first.width)
				c.second.append(RTLIL::SigSpec(RTLIL::State::S0, c.first.width - c.second.width));
			assert(c.first.width == c.second.width);
			module->connections.push_back(c);
		}

		delete cell;
		module->cells.erase(cell_name);
		did_something = true;
	next_cell:;
	}

	return did_something;
}

struct TechmapPass : public Pass {
	TechmapPass() : Pass("techmap") { }
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Executing TECHMAP pass (map to technology primitives).\n");
		log_push();

		std::string filename;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-map" && argidx+1 < args.size()) {
				filename = args[++argidx];
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		RTLIL::Design *map = new RTLIL::Design;
		FILE *f = filename.empty() ? fmemopen(stdcells_code, strlen(stdcells_code), "rt") : fopen(filename.c_str(), "rt");
		if (f == NULL)
			log_error("Can't open map file `%s'\n", filename.c_str());
		Frontend::frontend_call(map, f, filename.empty() ? "<stdcells.v>" : filename, "verilog");
		fclose(f);

		std::map<RTLIL::IdString, RTLIL::Module*> modules_new;
		for (auto &it : map->modules) {
			if (it.first.substr(0, 2) == "\\$")
				it.second->name = it.first.substr(1);
			modules_new[it.second->name] = it.second;
		}
		map->modules.swap(modules_new);

		bool did_something = true;
		while (did_something) {
			did_something = false;
			for (auto &mod_it : design->modules)
				if (techmap_module(mod_it.second, map))
					did_something = true;
		}

		log("No more expansions possible.\n");
		techmap_cache.clear();
		delete map;
		log_pop();
	}
} TechmapPass;
 
