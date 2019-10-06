//
//  AtariST.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 03/10/2019.
//  Copyright © 2019 Thomas Harte. All rights reserved.
//

#include "AtariST.hpp"

#include "../CRTMachine.hpp"

#include "../../Processors/68000/68000.hpp"

#include "Video.hpp"
#include "../../ClockReceiver/JustInTime.hpp"

#include "../Utility/MemoryPacker.hpp"
#include "../Utility/MemoryFuzzer.hpp"

namespace Atari {
namespace ST {

const int CLOCK_RATE = 8000000;

using Target = Analyser::Static::Target;

class ConcreteMachine:
	public Atari::ST::Machine,
	public CPU::MC68000::BusHandler,
	public CRTMachine::Machine {
	public:
		ConcreteMachine(const Target &target, const ROMMachine::ROMFetcher &rom_fetcher) :
			mc68000_(*this) {
			set_clock_rate(CLOCK_RATE);

			ram_.resize(512 * 512);
			Memory::Fuzz(ram_);

			std::vector<ROMMachine::ROM> rom_descriptions = {
				{"AtariST", "the TOS ROM", "tos100.img", 192*1024, 0x1a586c64}
			};
			const auto roms = rom_fetcher(rom_descriptions);
			if(!roms[0]) {
				throw ROMMachine::Error::MissingROMs;
			}
			Memory::PackBigEndian16(*roms[0], rom_);

			// Set up basic memory map.
			memory_map_[0] = BusDevice::MostlyRAM;
			for(int c = 1; c < 0xf0; ++c) memory_map_[c] = BusDevice::RAM;

			// This is appropriate for: TOS 1.x, no cartridge.
			for(int c = 0xf0; c < 0xfc; ++c) memory_map_[c] = BusDevice::Unassigned;
			for(int c = 0xfc; c < 0xff; ++c) memory_map_[c] = BusDevice::ROM;
			memory_map_[0xfa] = memory_map_[0xfb] = BusDevice::Cartridge;

			memory_map_[0xff] = BusDevice::IO;
		}

		// MARK: CRTMachine::Machine
		void set_scan_target(Outputs::Display::ScanTarget *scan_target) final {
			video_->set_scan_target(scan_target);
		}

		Outputs::Speaker::Speaker *get_speaker() final {
			return nullptr;
		}

		void run_for(const Cycles cycles) final {
			mc68000_.run_for(cycles);
		}

		// MARK: MC68000::BusHandler
		using Microcycle = CPU::MC68000::Microcycle;
		HalfCycles perform_bus_operation(const CPU::MC68000::Microcycle &cycle, int is_supervisor) {
			// Advance time.
			video_ += cycle.length;

			// A null cycle leaves nothing else to do.
			if(!(cycle.operation & (Microcycle::NewAddress | Microcycle::SameAddress))) return HalfCycles(0);

			/* TODO: DTack, bus error, VPA.  */

			auto address = cycle.word_address();
			uint16_t *memory;
			switch(memory_map_[address >> 15]) {
				case BusDevice::MostlyRAM:
					if(address < 4) {
						memory = rom_.data();
						break;
					}
				case BusDevice::RAM:
					memory = ram_.data();
					address &= ram_.size() - 1;
					// TODO: align with the next access window.
				break;

				case BusDevice::ROM:
					memory = rom_.data();
					address %= rom_.size();
				break;

				case BusDevice::Cartridge:
					/*
						TOS 1.0 appears to attempt to read from the catridge before it has setup
						the bus error vector. Therefore I assume no bus error flows.
					*/
					switch(cycle.operation & (Microcycle::SelectWord | Microcycle::SelectByte | Microcycle::Read)) {
						default: break;
						case Microcycle::SelectWord | Microcycle::Read:
							*cycle.value = 0xffff;
						break;
						case Microcycle::SelectByte | Microcycle::Read:
							cycle.value->halves.low = 0xff;
						break;
					}
				return HalfCycles(0);

				case BusDevice::Unassigned:
				return HalfCycles(0);

				case BusDevice::IO:
					assert(false);
				break;
			}

			// If control has fallen through to here, the access is either a read from ROM, or a read or write to RAM.
			switch(cycle.operation & (Microcycle::SelectWord | Microcycle::SelectByte | Microcycle::Read)) {
				default:
				break;

				case Microcycle::SelectWord | Microcycle::Read:
					cycle.value->full = memory[address];
				break;
				case Microcycle::SelectByte | Microcycle::Read:
					cycle.value->halves.low = uint8_t(memory[address] >> cycle.byte_shift());
				break;
				case Microcycle::SelectWord:
					memory[address] = cycle.value->full;
				break;
				case Microcycle::SelectByte:
					memory[address] = uint16_t(
						(cycle.value->halves.low << cycle.byte_shift()) |
						(memory[address] & cycle.untouched_byte_mask())
					);
				break;
			}

			return HalfCycles(0);
		}

	private:
		CPU::MC68000::Processor<ConcreteMachine, true> mc68000_;
		JustInTimeActor<Video, HalfCycles> video_;

		std::vector<uint16_t> ram_;
		std::vector<uint16_t> rom_;

		enum class BusDevice {
			MostlyRAM, RAM, ROM, Cartridge, IO, Unassigned
		};
		BusDevice memory_map_[256];
};

}
}

using namespace Atari::ST;

Machine *Machine::AtariST(const Analyser::Static::Target *target, const ROMMachine::ROMFetcher &rom_fetcher) {
	return new ConcreteMachine(*target, rom_fetcher);
}

Machine::~Machine() {}
