#include "CoinChanger.hpp"
#include <Arduino.h>

CoinChanger::CoinChanger(MDBSerial &mdb) : MDBDevice(mdb)
{
	ADDRESS = 0x08;
	STATUS = 0x02;
	DISPENSE = 0x05;
	
	m_acceptedCoins = 0xFFFF; //all coins enabled by default
	m_dispenseableCoins = 0xFFFF;
	
	m_resetCount = 0;
	
	m_credit = 0;
	m_coin_scaling_factor = 0;
	m_decimal_places = 0;
	m_coin_type_routing = 0;
	
	for (int i = 0; i < 16; i++)
	{
		m_coin_type_credit[i] = 0;
		m_tube_status[i] = 0;
	}
	m_tube_full_status = 0;
	
	m_software_version = 0;
	m_optional_features = 3;
	
	m_alternative_payout_supported = false;
	m_extended_diagnostic_supported = false;
	m_manual_fill_and_payout_supported = false;
	m_file_transport_layer_supported = false;
	
	m_update_count = 0;
	
	m_value_to_dispense = 0;
	m_dispensed_value = 0;
}

bool CoinChanger::Update(unsigned long &change, int it)
{
	poll();
	status();
	
	if ((m_update_count % 50) == 0)
	{
		if (m_feature_level >= 3)
		{
			expansion_send_diagnostic_status();
		}
		//Dispense(15, 1);
		m_update_count = 0;
	}

	if(m_feature_level >= 3)
		expansion_payout_status();
	
	
	if (m_dispensed_value >= m_value_to_dispense)
	{
		m_dispensed_value = 0;
		m_value_to_dispense = 0;
	}
	else
	{
		//to make sure we send the sms only once
		if (it == 0)
			severe << F("CC: DISPENSE FAILED") << endl;
		else
			error << F("CC: DISPENSE FAILED") << endl;
		return false;
	}
	
	change = 0;
	for (int i = 0; i < 16; i++)
	{
		change += m_coin_type_credit[i] * m_tube_status[i] * m_coin_scaling_factor;
	}
	
	type();
	
	m_update_count++;
	return true; 
}

bool CoinChanger::Reset()
{
	expansion_send_diagnostic_status(); //waits for dispenser to start
	
	m_mdb->SendCommand(ADDRESS, RESET);
	if ((m_mdb->GetResponse() == ACK))
	{
		int count = 0;
		while (poll() != JUST_RESET) 
		{
			if (count > MAX_RESET_POLL) return false;
			count++;
		}
		if (!setup())
			return false;
		status();
		Print();
		debug << F("CC: RESET COMPLETED") << endl;
		return true;
	}
	debug << F("CC: RESET FAILED") << endl;

	if (m_resetCount < MAX_RESET)
	{
		m_resetCount++;
		return Reset();
	}
	else
	{
		m_resetCount = 0;
		console << F("CC: NOT RESPONDING") << endl;
		return false;
	}
	return true;
}


bool CoinChanger::Dispense(unsigned long value)
{
	if (m_alternative_payout_supported)
	{
		m_value_to_dispense += value;
		char val = value / m_coin_scaling_factor;
		expansion_payout(val);
		return true;
	}
	else
	{
		warning << F("CC: OLD DISPENSE FUNCTION USED") << endl;
		for (int i = 15; i >= 0; i--)
		{
			int count = value / (m_coin_type_credit[i] * m_coin_scaling_factor);
			count = min(count, m_tube_status[i]); //check if sufficient coins in tube
			if (Dispense(i, count))
			{
				unsigned long val = count * m_coin_type_credit[i] * m_coin_scaling_factor;
				m_value_to_dispense += val;
				value -= val;
			}
			if (value <= 0)
				break;
		}
		
		if (value == 0)
			return true;
		
		if (value < 5)
			value = 5;
		else 
			value++;
		
		//possible deadloop
		return Dispense(value);
	}
}

bool CoinChanger::Dispense(int coin, int count)
{
	int out = (count << 4) | coin;
	m_mdb->SendCommand(ADDRESS, DISPENSE, &out, 1);
	if (m_mdb->GetResponse() != ACK)
	{
		warning << F("CC: DISPENSE FAILED") << endl;
		return false;
	}
	return true;
}

void CoinChanger::Print()
{
	debug << F("## CoinChanger ##") << endl;
	debug << F("credit: ") << m_credit << endl;
	debug << F("country: ") << m_country << endl;
	debug << F("feature level: ") << (int)m_feature_level << endl;
	debug << F("coin scaling factor: ") << (int)m_coin_scaling_factor << endl;
	debug << F("decimal places: ") << (int)m_decimal_places << endl;
	debug << F("coin type routing: ") << m_coin_type_routing << endl;
	
	debug << F("coin type credits: ");
	for (int i = 0; i < 16; i++)
	{
		debug << (int)m_coin_type_credit[i] << " ";
    }
    debug << endl;
	
	debug << F("tube full status: ") << (int)m_tube_full_status << endl;
	
	debug << F("tube status: ");
	for (int i = 0; i < 16; i++)
	{
		debug << (int)m_tube_status[i] << " ";
    }
	debug << endl;
	
	debug << F("software version: ") << m_software_version << endl;
	debug << F("optional features: ") << m_optional_features << endl;
	debug << F("alternative payout supported: ") << (bool)m_alternative_payout_supported << endl;
	debug << F("extended diagnostic supported: ") << (bool)m_extended_diagnostic_supported << endl;
	debug << F("mauall fill and payout supported: ") << (bool)m_manual_fill_and_payout_supported << endl;
	debug << F("file transport layer supported: ") << (bool)m_file_transport_layer_supported << endl;
	debug << F("######") << endl;
}

int CoinChanger::poll()
{
	bool reset = false;
	bool payout_busy = false;
	for (int i = 0; i < 64; i++)
		m_buffer[i] = 0;
	m_mdb->SendCommand(ADDRESS, POLL);
	m_mdb->Ack();
	int answer = m_mdb->GetResponse(m_buffer, &m_count, 16);
	if (answer == ACK)
	{
		return 1;
	}

	//max of 16 bytes as response
	for (int i = 0; i < m_count && i < 16; i++)
	{
		//coins dispensed manually
		if (m_buffer[i] & 0b10000000)
		{
			
			int count = (m_buffer[i] & 0b01110000) >> 4;
			int type = m_buffer[i] & 0b00001111;
			int coins_in_tube = m_buffer[i + 1];
			
			m_dispensed_value += m_coin_type_credit[type] * m_coin_scaling_factor * count;
			i++; //cause we used 2 bytes
		}
		//coins deposited
		else if (m_buffer[i] & 0b01000000)
		{
			int routing = (m_buffer[i] & 0b00110000) >> 4;
			int type = m_buffer[i] & 0b00001111;
			int those_coins_in_tube = m_buffer[i + 1];
			if (routing < 2)
			{
				m_credit += (m_coin_type_credit[type] * m_coin_scaling_factor);
			}
			else
			{
				debug << F("CC: coin rejected") << endl;
			}
			i++; //cause we used 2 bytes
		}
		//slug
		else if (m_buffer[i] & 0b00100000)
		{
			int slug_count = m_buffer[i] & 0b00011111;
			debug << F("CC: slug") << endl;
		}
		//status
		else
		{
			switch (m_buffer[i])
			{
			case 1:
				//escrow request
				break;
			case 2:
				//changer payout busy
				debug << F("CC: changer payout busy") << endl;
				payout_busy = true;
				break;
			case 3:
				//no credit
				break;
			case 4:
				//defective tube sensor
				error << F("CC: defective tube sensor") << endl;
				break;
			case 5:
				//double arrival
				debug << F("CC: double arrival") << endl;
				break;
			case 6:
				//acceptor unplugged
				break;
			case 7:
				//tube jam
				debug << F("CC: tube jam") << endl;
				break;
			case 8:
				//ROM checksum error
				warning << F("CC: ROM checksum error") << endl;
				break;
			case 9:
				//coin routing error
				debug << F("CC: coin routing error") << endl;
				break;
			case 10:
				//changer busy
				break;
			case 11:
				//changer was reset
				reset = true;
				break;
			case 12:
				//coin jam
				warning << F("CC: coin jam") << endl;
				break;
			case 13:
				//possible credited coin removal
				debug << F("CC: possible credited coin removal") << endl;
				break;
			default:
				debug << F("CC: default: ");
				for ( ; i < m_count; i++)
					debug << m_buffer[i];
				debug << endl;
			}
		}
	}
	if (reset)
		return JUST_RESET;
	if (payout_busy)
	{
		delay(2000);
		return poll();
	}
	return 1;
}

bool CoinChanger::setup(int it)
{
	m_mdb->SendCommand(ADDRESS, SETUP);
	int answer = m_mdb->GetResponse(m_buffer, &m_count, 23);
	if (answer >= 0 && m_count == 23)
	{
		m_mdb->Ack();
		m_feature_level = m_buffer[0];
		m_country = m_buffer[1] << 8 | m_buffer[2];
		m_coin_scaling_factor = m_buffer[3];
		m_decimal_places = m_buffer[4];
		m_coin_type_routing = m_buffer[5] << 8 | m_buffer[6];
		for (int i = 0; i < 16; i++)
		{
			m_coin_type_credit[i] = m_buffer[7 + i];
		}

		if (m_feature_level >= 3)
		{
			expansion_identification();
			expansion_feature_enable(m_optional_features);
		}
		return true;
	}
	if (it < 5)
	{
		delay(50);
		return setup(++it);
	}
	error << F("CC: SETUP ERROR") << endl;
	return false;
}

void CoinChanger::status(int it)
{
	m_mdb->SendCommand(ADDRESS, STATUS);
	int answer = m_mdb->GetResponse(m_buffer, &m_count, 18);
	if (answer != -1 && m_count == 18)
	{
		m_mdb->Ack();
		//if bit is set, the tube is full
		m_tube_full_status = m_buffer[0] << 8 | m_buffer[1];
		for (int i = 0; i < 16; i++)
		{
			//number of coins in the tube
			m_tube_status[i] = m_buffer[2 + i];
		}
		return;
	}
	if (it < 5)
	{
		delay(50);
		return status(++it);
	}
	warning << F("CC: STATUS ERROR") << endl;
}

void CoinChanger::type(int it)
{
	int out[] = { uint8_t((m_acceptedCoins & 0xff00) >> 8), 
					uint8_t(m_acceptedCoins & 0xff), 
					uint8_t((m_dispenseableCoins & 0xff00) >> 8), 
					uint8_t(m_dispenseableCoins & 0xff) };
					
	m_mdb->SendCommand(ADDRESS, TYPE, out, 4);
	if (m_mdb->GetResponse() != ACK)
	{
		if (it < 5)
		{
			delay(50);
			return type(++it);
		}
		error << F("CC: TYPE ERROR") << endl;
	}
}

void CoinChanger::expansion_identification(int it)
{
	m_mdb->SendCommand(ADDRESS, EXPANSION, IDENTIFICATION);
	int answer = m_mdb->GetResponse(m_buffer, &m_count, 33);
	if (answer > 0 && m_count == 33)
	{
		m_mdb->Ack();
		// * 1L to overcome 16bit integer error
		m_manufacturer_code = (m_buffer[0] * 1L) << 16 | m_buffer[1] << 8 | m_buffer[2];
		for (int i = 0; i < 12; i++)
		{
			m_serial_number[i] = m_buffer[3 + i];
			m_model_number[i] = m_buffer[15 + i];
		}

		m_software_version = m_buffer[27] << 8 | m_buffer[28];
		m_optional_features = (m_buffer[29] * 1L) << 24 | (m_buffer[30] * 1L) << 16 | m_buffer[31] << 8 | m_buffer[32];

		if (m_optional_features & 0b1)
		{
			m_alternative_payout_supported = true;
		}
		if (m_optional_features & 0b10)
		{
			m_extended_diagnostic_supported = true;
		}
		if (m_optional_features & 0b100)
		{
			m_manual_fill_and_payout_supported = true;
		}
		if (m_optional_features & 0b1000)
		{ 
			m_file_transport_layer_supported = true;
		}
		return;
	}
	if (it < 5)
	{
		delay(50);
		expansion_identification(++it);
	}
	error << F("CC: EXP ID ERROR") << endl;
}

void CoinChanger::expansion_feature_enable(int features)
{
	int out[] = { 0xff, 0xff, 0xff, 0xff };
	m_mdb->SendCommand(ADDRESS, EXPANSION, FEATURE_ENABLE, out, 4);
}

void CoinChanger::expansion_payout(int value)
{
	//expansion_feature_enable(1);
	int val = 1;
	//m_mdb->SendCommand(ADDRESS, EXPANSION, PAYOUT, &val, 1);
	Dispense(15, 1);
}

//number of each coin dispensed since last alternative payout (expansion_payout)
//-> response to alternative payout (expansion_payout)
//changer clears output data after an ack from controller
void CoinChanger::expansion_payout_status(int it)
{
	m_mdb->SendCommand(ADDRESS, EXPANSION, PAYOUT_STATUS);
	int answer = m_mdb->GetResponse(m_buffer, &m_count, 16);
	if (answer == ACK) //payout busy
	{
		m_mdb->Ack();
		if (it < 5)
		{	
			delay(500);		
			expansion_payout_status(++it);
		}
		return;
	}
	else if (answer > 0 && m_count == 16)
	{
		for (int i = 0; i < 16; i++)
		{
			int count = m_buffer[i];
			m_dispensed_value += m_coin_type_credit[i] * m_coin_scaling_factor * count;
		}
	}
}

//scaled value that indicates the amount of change payd out since last poll or
//after the initial alternative_payout
long CoinChanger::expansion_payout_value_poll()
{
	m_mdb->SendCommand(ADDRESS, EXPANSION, PAYOUT_VALUE_POLL);
	int answer = m_mdb->GetResponse(m_buffer, &m_count, 1);
	if (answer == ACK) //payout finished 
		expansion_payout_status();
	else if (answer > 0 && m_count == 1)
	{
		return m_buffer[0];
	}
	return 0;
}

//should be send by the vmc every 1-10 seconds
void CoinChanger::expansion_send_diagnostic_status()
{
	bool powering_up = false;
	
	m_mdb->SendCommand(ADDRESS, EXPANSION, SEND_DIAGNOSTIC_STATUS);
	int answer = m_mdb->GetResponse(m_buffer, &m_count, 2);
	
	if (answer > 0 && m_count >= 2)
	{		
		m_mdb->Ack();
		
		for (int i = 0; i < m_count / 2; i++)
		{
			switch (m_buffer[0])
			{
			case 1:
				debug <<  F("CC: powering up") << endl;
				powering_up = true;
				break;
			case 2:
				debug <<  F("CC: powering down") << endl;
				break;
			case 3:
				debug <<  F("CC: OK") << endl;
				break;
			case 4:
				debug <<  F("CC: keypad shifted") << endl;
				break;
				
			case 5:
				switch (m_buffer[1])
				{
				case 10:
					debug <<  F("CC: manual fill / payout active") << endl;
					break;
				case 20:
					debug <<  F("CC: new inventory information available") << endl;
					break;
				}
				break;
				
			case 6:
				debug << F("CC: inhibited by VMC") << endl;
				break;
				
			case 10:
				switch(m_buffer[1])
				{
				case 0:
					error << F("CC: non specific error") << endl;
					break;
				case 1:
					error << F("CC: check sum error #1") << endl;
					break;
				case 2:
					error << F("CC: check sum error #2") << endl;
					break;
				case 3:
					error << F("CC: low line voltage detected") << endl;
					break;
				}
				break;
				
			case 11:
				switch(m_buffer[1])
				{
					case 0:
						error << F("CC: non specific discriminator error") << endl;
						break;
					case 10:
						error << F("CC: flight deck open") << endl;
						break;
					case 11:
						error << F("CC: escrow return stuck open") << endl;
						break;
					case 30:
						error << F("CC: coin jam in sensor") << endl;
						break;
					case 41:
						error << F("CC: discrimination below specified standard") << endl;
						break;
					case 50:
						error << F("CC: validation sensor A out of range") << endl;
						break;
					case 51:
						error << F("CC: validation sensor B out of range") << endl;
						break;
					case 52:
						error << F("CC: validation sensor C out of range") << endl;
						break;
					case 53:
						error << F("CC: operation temperature exceeded") << endl;
						break;
					case 54:
						error << F("CC: sizing optics failure") << endl;
						break;
				}
				break;
			
			case 12:
				switch(m_buffer[1])
				{
					case 0:
						error << F("CC: non specific accept gate error") << endl;
						break;
					case 30:
						error << F("CC: coins entered gate, but did not exit") << endl;
						break;
					case 31:
						error << F("CC: accept gate alarm active") << endl;
						break;
					case 40:
						error << F("CC: accept gate open, but no coin detected") << endl;
						break;
					case 50:
						error << F("CC: post gate sensor covered before gate openend") << endl;
						break;
				}
				break;
			
			case 13:
				switch(m_buffer[1])
				{
					case 0:
						error << F("CC: non specific separator error") << endl;
						break;
					case 10:
						error << F("CC: sort sensor error") << endl;
						break;
				}
				break;

			case 14:
				switch(m_buffer[1])
				{
					case 0:
						error << F("CC: non specific dispenser error") << endl;
						break;
				}
				break;
				
			case 15:
				switch(m_buffer[1])
				{
					case 0:
						error << F("CC: non specific cassette error") << endl;
						break;
					case 2:
						error << F("CC: cassette removed") << endl;
						break;
					case 3:
						error << F("CC: cash box sensor error") << endl;
						break;
					case 4:
						error << F("CC: sunlight on tube sensors") << endl;
						break;
				}
				break;
			}
		}
		
		if (powering_up)
		{
			delay(1000);
			expansion_send_diagnostic_status();
		}
	}
}