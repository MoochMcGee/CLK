//
//  IWM.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 05/05/2019.
//  Copyright © 2019 Thomas Harte. All rights reserved.
//

#include "IWM.hpp"

#include "../../Outputs/Log.hpp"

using namespace Apple;

namespace  {
	const int CA0		= 1 << 0;
	const int CA1		= 1 << 1;
	const int CA2		= 1 << 2;
	const int LSTRB		= 1 << 3;
	const int ENABLE	= 1 << 4;
	const int DRIVESEL	= 1 << 5;	/* This means drive select, like on the original Disk II. */
	const int Q6		= 1 << 6;
	const int Q7		= 1 << 7;
	const int SEL		= 1 << 8;	/* This is an additional input, not available on a Disk II, with a confusingly-similar name to SELECT but a distinct purpose. */
}

IWM::IWM(int clock_rate) :
	clock_rate_(clock_rate) {}

// MARK: - Bus accessors

uint8_t IWM::read(int address) {
	access(address);

	// Per Inside Macintosh:
	//
	// "Before you can read from any of the disk registers you must set up the state of the IWM so that it
	// can pass the data through to the MC68000's address space where you'll be able to read it. To do that,
	// you must first turn off Q7 by reading or writing dBase+q7L. Then turn on Q6 by accessing dBase+q6H.
	// After that, the IWM will be able to pass data from the disk's RD/SENSE line through to you."
	//
	// My understanding:
	//
	//	Q6 = 1, Q7 = 0 reads the status register. The meaning of the top 'SENSE' bit is then determined by
	//	the CA0,1,2 and SEL switches as described in Inside Macintosh, summarised above as RD/SENSE.

	if(address&1) {
		return 0xff;
	}

	switch(state_ & (Q6 | Q7 | ENABLE)) {
		default:
			LOG("[IWM] Invalid read\n");
		return 0xff;

		// "Read all 1s".
//			printf("Reading all 1s\n");
//		return 0xff;

		case 0:
		case ENABLE: {				/* Read data register. Zeroing afterwards is a guess. */
			const auto result = data_register_;

			if(data_register_ & 0x80) {
//				printf("\n\nIWM:%02x\n\n", data_register_);
				data_register_ = 0;
			}
//			LOG("Reading data register: " << PADHEX(2) << int(result));

			return result;
		}

		case Q6: case Q6|ENABLE: {
			/*
				[If A = 0], Read status register:

				bits 0-4: same as mode register.
				bit 5: 1 = either /ENBL1 or /ENBL2 is currently low.
				bit 6: 1 = MZ (reserved for future compatibility; should always be read as 0).
				bit 7: 1 = SENSE input high; 0 = SENSE input low.

				(/ENBL1 is low when the first drive's motor is on; /ENBL2 is low when the second drive's motor is on.
				If the 1-second timer is enabled, motors remain on for one second after being programmatically disabled.)
			*/
//			LOGNBR("Reading status (including [" << active_drive_ << "][" << ((state_ & CA2) ? '2' : '-') << ((state_ & CA1) ? '1' : '-') << ((state_ & CA0) ? '0' : '-') << ((state_ & SEL) ? 'S' : '-') << "] ");

			// Determine the SENSE input.
			uint8_t sense = 1;
			if(drives_[active_drive_])
				sense = drives_[active_drive_]->read() ? 1 : 0;

			return uint8_t(
				(mode_&0x1f) |
				((state_ & ENABLE) ? 0x20 : 0x00) |
				(sense << 7)
			);
		} break;

		case Q7: case Q7|ENABLE:
			/*
				Read write-handshake register:

				bits 0-5: reserved for future use (currently read as 1).
				bit 6: 1 = write state (cleared to 0 if a write underrun occurs).
				bit 7: 1 = write data buffer ready for data.
			*/
			LOG("Reading write handshake");
		return 0x1f | 0x80 | 0x40;
	}

	return 0xff;
}

void IWM::write(int address, uint8_t input) {
	access(address);

	switch(state_ & (Q6 | Q7 | ENABLE)) {
		default: break;

		case Q7|Q6:
			/*
				Write mode register:

				bit 0: 1 = latch mode (should be set in asynchronous mode).
				bit 1: 0 = synchronous handshake protocol; 1 = asynchronous.
				bit 2: 0 = 1-second on-board timer enable; 1 = timer disable.
				bit 3: 0 = slow mode; 1 = fast mode.
				bit 4: 0 = 7Mhz; 1 = 8Mhz (7 or 8 mHz clock descriptor).
				bit 5: 1 = test mode; 0 = normal operation.
				bit 6: 1 = MZ-reset.
				bit 7: reserved for future expansion.
			*/

			mode_ = input;

			switch(mode_ & 0x18) {
				case 0x00:		bit_length_ = Cycles(24);		break;	// slow mode, 7Mhz
				case 0x08:		bit_length_ = Cycles(12);		break;	// fast mode, 7Mhz
				case 0x10:		bit_length_ = Cycles(32);		break;	// slow mode, 8Mhz
				case 0x18:		bit_length_ = Cycles(16);		break;	// fast mode, 8Mhz
			}
			LOG("IWM mode is now " << PADHEX(2) << int(mode_));
		break;

		case Q7|Q6|ENABLE:	// Write data register.
			LOG("Data register write\n");
		break;
	}
}

// MARK: - Switch access

void IWM::access(int address) {
	// Keep a record of switch state; bits in state_
	// should correlate with the anonymous namespace constants
	// defined at the top of this file — CA0, CA1, etc.
	address &= 0xf;
	const auto mask = 1 << (address >> 1);
	const auto old_state = state_;

	if(address & 1) {
		state_ |= mask;
	} else {
		state_ &= ~mask;
	}

	// React appropriately to motor requests and to LSTRB register writes.
	if(old_state != state_) {
		push_drive_state();

		switch(mask) {
			default: break;

			case ENABLE:
				if(address & 1) {
					if(drives_[active_drive_]) drives_[active_drive_]->set_enabled(true);
				} else {
					// If the 1-second delay is enabled, set up a timer for that.
//					if(!(mode_ & 4)) {
//						cycles_until_motor_off_ = Cycles(clock_rate_);
//					} else {
//						drive_motor_on_ = false;
						if(drives_[active_drive_]) drives_[active_drive_]->set_enabled(false);
//					}
				}
			break;

			case DRIVESEL: {
				const int new_drive = address & 1;
				if(new_drive != active_drive_) {
					if(drives_[active_drive_]) drives_[active_drive_]->set_enabled(false);
					active_drive_ = new_drive;
					if(drives_[active_drive_]) {
						drives_[active_drive_]->set_enabled(state_ & ENABLE);
						push_drive_state();
					}
				}
			} break;
		}
	}
}

void IWM::set_select(bool enabled) {
	// Store SEL as an extra state bit.
	if(enabled) state_ |= SEL;
	else state_ &= ~SEL;
	push_drive_state();
}

void IWM::push_drive_state() {
	if(drives_[active_drive_])  {
		const uint8_t drive_control_lines =
			((state_ & CA0) ? IWMDrive::CA0 : 0) |
			((state_ & CA1) ? IWMDrive::CA1 : 0) |
			((state_ & CA2) ? IWMDrive::CA2 : 0) |
			((state_ & SEL) ? IWMDrive::SEL : 0) |
			((state_ & LSTRB) ? IWMDrive::LSTRB : 0);
		drives_[active_drive_]->set_control_lines(drive_control_lines);
	}
}

// MARK: - Active logic

void IWM::run_for(const Cycles cycles) {
	// Check for a timeout of the motor-off timer.
//	if(cycles_until_motor_off_ > Cycles(0)) {
//		cycles_until_motor_off_ -= cycles;
//		if(cycles_until_motor_off_ <= Cycles(0)) {
//			drive_motor_on_ = false;
//			if(drives_[active_drive_])
//				drives_[active_drive_]->set_motor_on(false);
//		}
//	}

	// Activity otherwise depends on mode and motor state.
	const bool run_disk = drives_[active_drive_];	// drive_motor_on_ &&
	int integer_cycles = cycles.as_int();
	switch(state_ & (Q6 | Q7 | ENABLE)) {
		case 0:
		case ENABLE:	// i.e. read mode.
			while(integer_cycles--) {
				if(run_disk) {
					drives_[active_drive_]->run_for(Cycles(1));
				}
				++cycles_since_shift_;
				if(cycles_since_shift_ == bit_length_ + Cycles(2)) {
					propose_shift(0);
				}
			}
		break;

		default:
			if(run_disk) drives_[active_drive_]->run_for(cycles);
		break;
	}
}

void IWM::process_event(const Storage::Disk::Drive::Event &event) {
	switch(event.type) {
		case Storage::Disk::Track::Event::IndexHole: return;
		case Storage::Disk::Track::Event::FluxTransition:
			propose_shift(1);
		break;
	}
}

void IWM::propose_shift(uint8_t bit) {
	// TODO: synchronous mode.

	shift_register_ = uint8_t((shift_register_ << 1) | bit);
	if(shift_register_ & 0x80) {
		data_register_ = shift_register_;
		shift_register_ = 0;
	}
	cycles_since_shift_ = Cycles(0);
}

void IWM::set_drive(int slot, IWMDrive *drive) {
	drives_[slot] = drive;
	drive->set_event_delegate(this);
}
