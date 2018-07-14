//
// Created by JONGWON on 2018-07-09.
//

// MHDR(1) [MType(3) | RFU(3) | Major(2) ]
// MACPayload(7~N) [ FHDR(7~22) | FPort(0-1) | FRMPayload(0-N)]
// MIC(4)

#define LORAWAN_MAC_
#define LORAWAN_MAC_HEADER_LEN					1			/**< MAC Header Length */
#define LORAWAN_MAC_PAYLOAD_LEN_MIN				7			/**< MAC Payload Minimum Length */

/** MType message types  */

#define JOIN_REQUEST							0x00	 	/**< Join Request Message from End-Device */
#define JOIN_ACCEPT								0x01		/**< Join Accept Message  */
#define UNCONFIRMED_DATA_UP						0x02		/**< Unconfirmed Data Up Message from End-Device */
#define UNCONFIRMED_DATA_DOWN					0x03		/**< Unconfirmed Data Down Message from GateWay */
#define CONFIRMED_DATA_UP						0x04		/**< Confirmed Data Up Message from End-Device */
#define CONFIRMED_DATA_DOWN						0x05		/**< Confirmed Data Down Message from GateWay */
#define MTYPE_RFU								0x06		/**< Mtype Reserved for future use  */		
#define PROPRIETARY								0x07		/**< Proprietary message from End-Device?  */

typedef struct LorawanMacHdr_ {
	unsigned int mtype : 3;
	unsigned int rfu : 3;
	unsigned int major : 2;
} LorawanMacHdr;

