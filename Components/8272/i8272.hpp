//
//  i8272.hpp
//  Clock Signal
//
//  Created by Thomas Harte on 05/08/2017.
//  Copyright © 2017 Thomas Harte. All rights reserved.
//

#ifndef i8272_hpp
#define i8272_hpp

#include <cstdint>
#include <vector>

namespace Intel {

class i8272 {
	public:
		void set_register(int address, uint8_t value);
		uint8_t get_register(int address);

	private:
		uint8_t status_;
		std::vector<uint8_t> command_;
};

}


#endif /* i8272_hpp */
