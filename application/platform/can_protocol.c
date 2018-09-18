/*
 *  Author: Adam Huang
 *  Date:2017/01/23
 */

#include "can_protocol.h"
#include "platform.h"

#include "stm32f1xx.h"
#include "Debug.h"
#include "UltraSonic.h"
#include "fifo.h"
#include "upgrade_flash.h"

#define COM_ERR_TEST    0
#if COM_ERR_TEST
#include <stdlib.h>
#define COM_ERR_RAND_NUM  300
#define COM_ERR_ACK_RAND_NUM  4
#endif

#define can_protocol_log(format, ...)  custom_log("can protocol", format, ##__VA_ARGS__)

__IO uint32_t flag = 0xff;

CanTxMsgTypeDef tx_message;

extern platform_can_driver_t  platform_can_drivers[];
uint8_t can_tx_buf[CAN_LONG_FRAME_LENTH_MAX] = {0};


can_fifo_t can_fifo_ram;
can_fifo_t *can_fifo = &can_fifo_ram;

can_pkg_t can_pkg[CAN_FIFO_SIZE] = {0};

extern uint8_t get_key_value(mico_gpio_t gpio);

uint8_t get_my_src_mac_id(void)
{

#define DEBOUNCE_TIME       100/SYSTICK_PERIOD
    uint8_t new_key_value = 0;
    uint8_t old_key_value = 0;

    static uint32_t start_time = 0;
    start_time = os_get_time();
    while(os_get_time() - start_time <= DEBOUNCE_TIME)
    {
        old_key_value = new_key_value;

        new_key_value |=  get_key_value(MICO_GPIO_KEY_S0);
        new_key_value |=  get_key_value(MICO_GPIO_KEY_S1) << 1;
        new_key_value |=  get_key_value(MICO_GPIO_KEY_S2) << 2;
        new_key_value |=  get_key_value(MICO_GPIO_KEY_S3) << 3;
        new_key_value |=  (get_key_value(MICO_GPIO_KEY_S4) == 1? 0:1) << 4;

        if(new_key_value != old_key_value)
        {
            start_time = os_get_time();
        }
    }

    if((new_key_value != 0) && (new_key_value <= 0x1f))
    {
        return new_key_value + ULTRASONIC_SRC_ID_BASE - 1;
    }
    can_protocol_log("Ultrasonic CAN MAC ID out of range ! ! ! \r\n");
    return 0xff;

}


#define ONLYONCE       0x00
#define BEGIAN         0x01
#define TRANSING       0x02
#define END            0x03

void tx_can_data(mico_can_t can_type, uint32_t canx_id,uint8_t* pdata,uint16_t len)
{
    uint16_t t_len;
    uint16_t round_count;
    uint8_t mod_count;
    can_data_union tx_msg = {0};
    //CanTxMsgTypeDef *tx_message = platform_can_drivers[can_type].handle->pTxMsg;

    t_len = len;
    round_count = t_len/7;
    mod_count = t_len%7;

    tx_message.ExtId = canx_id;
    tx_message.IDE   = CAN_ID_EXT;          //扩展模式
    tx_message.RTR   = CAN_RTR_DATA;        //发送的是数据
    //if(round_count <= 1)
    if(t_len <= 7)
    {
        tx_msg.can_data_t.seg_polo = ONLYONCE;
        tx_message.DLC = t_len+1;


        memcpy(&tx_message.Data[1],pdata,t_len);
        tx_message.Data[0] = tx_msg.can_data[0];

        if((CAN_USED->TSR&0x1C000000))
        {
            tx_can_pkg(MICO_CAN1, &tx_message);//
        }
        else
        {
            can_protocol_log("TX busy ! \r\n");
        }
        return ;
    }

    {
        int num;
        {
            for(num = 0; num < round_count; num++)
            {
                //SET SEGPOLO
                if( num == 0)
                {
                    tx_msg.can_data_t.seg_polo = BEGIAN;
                }
                else
                {
                    tx_msg.can_data_t.seg_polo = TRANSING;
                }

                if( mod_count == 0 && num == round_count-1)
                {
                    tx_msg.can_data_t.seg_polo = END;
                }

                tx_msg.can_data_t.seg_num = num;
                memcpy(tx_msg.can_data_t.Data, &pdata[num*7], 7);
                memcpy(tx_message.Data, tx_msg.can_data, 8);
                tx_message.DLC = 8;
                if((CAN_USED->TSR&0x1C000000))
                {
                    tx_can_pkg(MICO_CAN1, &tx_message);//发送报文
                }
                else
                {
                    can_protocol_log("TX busy ! \r\n");
                }

                //TRANSMIT LAST MSG
                if( mod_count !=0 && num == round_count - 1 )
                {
                    num++;
                    tx_msg.can_data_t.seg_polo = END;
                    tx_msg.can_data_t.seg_num = num;
                    memcpy(tx_msg.can_data_t.Data, &pdata[num*7], mod_count);
                    memcpy(tx_message.Data, tx_msg.can_data, mod_count+1);
                    tx_message.DLC = mod_count+1;
                    if((CAN_USED->TSR&0x1C000000))
                    {
                        tx_can_pkg(MICO_CAN1, &tx_message);//
                    }
                    else
                    {
                        can_protocol_log("TX busy ! \r\n");
                    }
                }
            }
        }
    }
}


#define CMD_NOT_FOUND   0
extern uint32_t ultrasonic_src_id;
static uint32_t can_test_cnt = 0;

uint16_t proc_can_cmd(can_id_union *id, const uint8_t *data_in, const uint16_t data_in_len, uint8_t *data_out)
{

    id->canx_id_t.ack = 1;
    id->canx_id_t.dest_mac_id = id->canx_id_t.src_mac_id;
    id->canx_id_t.src_mac_id = ultrasonic_src_id;
    id->canx_id_t.res = 0;

    switch(id->canx_id_t.func_id)
    {
        case CAN_FUN_ID_RESET:
            platform_mcu_reset();
            break;
        case CAN_FUN_ID_WRITE:
        case CAN_FUN_ID_READ:
            switch(id->canx_id_t.source_id)
            {
                case CAN_SOURCE_ID_READ_VERSION:
                    if(data_in_len == 1)
                    {
                        memcpy(&data_out[1], SW_VERSION, sizeof(SW_VERSION));
                        data_out[0] = strlen(SW_VERSION);
                        return (data_out[0] + 1);
                    }

                    break;
                case CAN_SOURCE_ID_READ_MEASURE_DATA:
                    if(ultra_sonic_data->start_flag == 0)
                    {
                        //ultrasonic_start();
                    }

                    return 0;
                    break;
                case CAN_SOURCE_ID_MEASUREMENT_EN:
                    if(data_in_len == 1)
                    {
                        if(data_in[0] == 1)
                        {
                            ultra_sonic_data->i_am_en = true;
                        }
                        else if(data_in[0] == 0)
                        {
                            ultra_sonic_data->i_am_en = false;
                        }
                        data_out[0] = data_in[0];
                        return 1;
                    }

                    break;

                case CAN_SOURCE_ID_SET_GROUP:
                    if(data_in_len == 1)
                    {
                        ultra_sonic_data->group = data_in[0];
                        data_out[0] = ultra_sonic_data->group;
                        return 1;
                    }

                    break;

                case CAN_SOURCE_ID_CAN_TEST:
                    can_test_cnt++;
                    memcpy(&data_out[0], (uint8_t *)&can_test_cnt, sizeof(can_test_cnt));
                    return sizeof(can_test_cnt);
                default :
                    break;
            }

        default:
            break;
    }

    return CMD_NOT_FOUND;
}

can_long_buf_t can_long_frame_buf_ram;
can_long_buf_t *can_long_frame_buf = &can_long_frame_buf_ram;

#define CAN_LONG_BUF_FULL   0xff
static uint8_t get_one_free_buf(void)
{
    for(uint8_t i = 0; i < CAN_LONG_BUF_NUM; i++)
    {
        if(can_long_frame_buf->can_rcv_buf[i].used_len == 0)
        {
            return i;
        }
    }
    return CAN_LONG_BUF_FULL;
}

static void free_buf(uint8_t index)
{
    can_long_frame_buf->can_rcv_buf[index].can_id = 0;
    can_long_frame_buf->can_rcv_buf[index].used_len = 0;
}

#define CAN_BUF_NO_THIS_ID      0xfe
static uint8_t get_the_buf_by_id(uint32_t id)
{
    for(uint8_t i = 0; i < CAN_LONG_BUF_NUM; i++)
    {
        if(id == can_long_frame_buf->can_rcv_buf[i].can_id)
        {
            return i;
        }
    }
    return CAN_BUF_NO_THIS_ID;
}

void can_long_buf_init(void)
{
    can_long_frame_buf->get_one_free_buf = get_one_free_buf;
    can_long_frame_buf->get_the_buf_by_id = get_the_buf_by_id;
    can_long_frame_buf->free_buf = free_buf;

    fifo_init(can_fifo, can_pkg, CAN_FIFO_SIZE);
}

void can_ack_back(uint32_t canx_id, const uint8_t * const pdata, uint16_t len)
{
    uint16_t t_len;
    CanTxMsgTypeDef tx_message;
    can_id_union id;
    uint8_t src_mac_id_temp;
    can_data_union tx_msg;

    id.canx_id = canx_id;
    id.canx_id_t.ack = 1;
    src_mac_id_temp = id.canx_id_t.dest_mac_id;
    id.canx_id_t.dest_mac_id = id.canx_id_t.src_mac_id;
    id.canx_id_t.src_mac_id = src_mac_id_temp;

    tx_message.ExtId = id.canx_id;
    tx_message.IDE   = CAN_ID_EXT;          //扩展模式
    tx_message.RTR   = CAN_RTR_DATA;        //发送的是数据

    t_len = len;
    if( t_len <=7 )
    {
        tx_msg.can_data_t.seg_polo = ONLYONCE;
        tx_msg.can_data_t.seg_num = 0;
        memcpy( tx_msg.can_data_t.Data, (const void *)pdata, t_len );
        memcpy( tx_message.Data, tx_msg.can_data, t_len + 1 );

        tx_message.DLC = t_len + 1;
        if( (CAN_USED->TSR & 0x1C000000) )
        {
            tx_can_pkg(MICO_CAN1, &tx_message );//
        }
    }
}

static OSStatus prepare_upgrade_process(can_id_union id, uint8_t *md5, uint8_t *firmware_Size )
{
    OSStatus err = kNoErr;
    uint32_t firmware_size;
    mico_logic_partition_t *ota_partition_info;
    uint8_t ack;

    ota_partition_info = MicoFlashGetInfo( MICO_PARTITION_OTA_TEMP );
    require_action( ota_partition_info->partition_owner != MICO_FLASH_NONE, exit, err = kUnsupportedErr );

    firmware_size = ReadBig32(firmware_Size);
    if( firmware_size > ota_partition_info->partition_length )
    {
        ack = 0x01;
        can_ack_back(id.canx_id, &ack, 1);
        can_protocol_log( "not enough storage" );
        goto exit;
    }
    can_protocol_log( "firmware_size is:%d", firmware_size );
    if( !prepare_upgrade_flash( md5, firmware_size ) )
    {
        ack = 0x00;
        can_ack_back(id.canx_id, &ack, 1);
        can_protocol_log( "mcu prepare ok" );
    }
    else
    {
        ack = 0x02;
        can_ack_back(id.canx_id, &ack, 1);
        can_protocol_log( "mcu retry later" );
    }

exit:
    return err;
}

static OSStatus rcv_upgrade_firmware_proc( can_id_union *id,  uint8_t *rx_data, uint8_t dataLen)
{
    OSStatus err = kNoErr;
    uint32_t packageDataLength = dataLen;
    uint8_t ack;

    static uint8_t index = 0;
    static uint8_t group = 0;

    if((group == rx_data[1]) && (index == rx_data[0]/* & 0x3f*/) && (index + group > 0))
    {
#if COM_ERR_TEST
        uint32_t rand_test;
        rand_test = rand() % COM_ERR_ACK_RAND_NUM;
        if(rand_test != COM_ERR_ACK_RAND_NUM>>1)
#endif
        {
            ack = 0x00;
            can_ack_back(id->canx_id, rx_data, 2);
        }
    }
    else
    {
        if( !upgradeWriteFlashData( (uint32_t *)(rx_data + 2), packageDataLength - 2 ) )
        {
#if COM_ERR_TEST
            uint32_t rand_test;
            rand_test = rand() % COM_ERR_RAND_NUM;
            if(rand_test != COM_ERR_RAND_NUM>>1)
#endif
            {
                ack = 0x00;
                can_ack_back(id->canx_id, rx_data, 2);
            }
        }
        else
        {
            ack = 0x01;
            can_ack_back(id->canx_id, &ack, 1);
            can_protocol_log( "mcu write data failed" );
            //index = rx_data[0] & 0x3f;
            //group = rx_data[1];
            goto exit;
        }
        index = rx_data[0]/* & 0x3f*/;
        group = rx_data[1];
    }

exit:
    return err;
}

static OSStatus check_upgrade_finish_proc( can_id_union *id )
{
    OSStatus err = kNoErr;
    uint8_t ack;

    if( !check_upgrade_finish() )
    {
        ack = 0x00;
        can_ack_back(id->canx_id, &ack, 1);
        can_protocol_log("MD5 success,sent right ack");
        platform_mcu_reset();
    }
    else
    {
        ack = 0x01;
        can_ack_back(id->canx_id, &ack, 1);
        can_protocol_log("MD5 err,sent err ack");
    }

    return err;
}


#define CAN_LONG_FRAME_TIME_OUT     5000/SYSTICK_PERIOD

#define CAN_COMM_TIME_OUT           1000
uint32_t can_comm_start_time;
void can_protocol_period( void )
{

    if(os_get_time() - can_comm_start_time >= CAN_COMM_TIME_OUT)
    {
        HAL_CAN_DeInit(platform_can_drivers[MICO_CAN1].handle);
        init_can( MICO_CAN1 );
        can_comm_start_time = os_get_time();
        //ENABLE_INTERRUPTS();
    }

    while(is_fifo_empty(can_fifo) == FALSE)
    {
        can_id_union id;
        can_pkg_t can_pkg_tmp;
        uint16_t tx_len;
        can_data_union rx_buf;
        uint8_t buf_index;
        uint8_t seg_polo;
        uint8_t seg_num;
        uint8_t rx_data_len;

        get_can_pkg_from_fifo(can_fifo, &can_pkg_tmp);

        memcpy(rx_buf.can_data,  can_pkg_tmp.data.can_data, can_pkg_tmp.len);
        id.canx_id = can_pkg_tmp.id.canx_id;
        seg_polo = can_pkg_tmp.data.can_data_t.seg_polo;
        seg_num = can_pkg_tmp.data.can_data_t.seg_num;
        rx_data_len = can_pkg_tmp.len;
        //can_comm_start_time = os_get_time();

        if( id.canx_id_t.source_id == 0x10 )//update_prepare
        {
            static uint8_t md5[16];
            static uint8_t firmware_size[4];

            //memcpy( &rx_buf, RxMessage.Data, RxMessage.DLC );
            if( rx_buf.can_data_t.seg_polo == BEGIAN )
            {
                memcpy(&md5[0], rx_buf.can_data_t.Data, 7 );
            }
            if( rx_buf.can_data_t.seg_polo == TRANSING )
            {
                memcpy(&md5[7], rx_buf.can_data_t.Data, 7 );
            }
            if( rx_buf.can_data_t.seg_polo == END )
            {
                memcpy(&md5[14], &rx_buf.can_data_t.Data[0], 2 );
                memcpy(&firmware_size[0], &rx_buf.can_data_t.Data[2], 4 );
                prepare_upgrade_process(id, md5, firmware_size);
            }
            //goto exit;
            continue;
        }
        if( id.canx_id_t.source_id == 0x11 )//update_receicing
        {
#if COM_ERR_TEST
            uint32_t rand_test;
            rand_test = rand() % COM_ERR_RAND_NUM;
            if(rand_test != COM_ERR_RAND_NUM>>1)
#endif
                rcv_upgrade_firmware_proc(&id, rx_buf.can_data, rx_data_len);

            //goto exit;
            continue;

        }
        if( id.canx_id_t.source_id == 0x12 )//update_finish_check
        {
            check_upgrade_finish_proc(&id);
            //goto exit;
            continue;
        }

        if(seg_polo == ONLYONCE)
        {
            //if( (id.canx_id_t.source_id < SOURCE_ID_PREPARE_UPDATE) && (id.canx_id_t.source_id > SOURCE_ID_CHECK_TRANSMIT) )
            if(id.canx_id_t.dest_mac_id == ultrasonic_src_id)
            {
                tx_len = proc_can_cmd(&id, rx_buf.can_data_t.Data, rx_data_len - 1, can_tx_buf );
                //process the data here//

                if(tx_len > 0)
                {
                    tx_can_data( MICO_CAN1, id.canx_id, can_tx_buf, tx_len );
                }
            }
        }
        else //long frame
        {
            for(uint8_t i = 0; i < CAN_LONG_BUF_NUM; i++)
            {
                if(can_long_frame_buf->can_rcv_buf[i].used_len > 0)
                {
                    if(os_get_time() - can_long_frame_buf->can_rcv_buf[i].start_time > CAN_LONG_FRAME_TIME_OUT)
                    {
                        can_long_frame_buf->free_buf(i);
                    }
                }
            }

            if(seg_polo == BEGIAN)
            {
                buf_index = can_long_frame_buf->get_the_buf_by_id(id.canx_id);
                if(buf_index == CAN_BUF_NO_THIS_ID)
                {
                    buf_index = can_long_frame_buf->get_one_free_buf();
                }
                else
                {
                    //
                }

                if((buf_index == CAN_LONG_BUF_FULL) || (buf_index >= CAN_LONG_BUF_NUM))
                {
                    can_protocol_log("LONG FRAME RCV BUF IS FULL! ! ! !\r\n");

                    goto exit;
                }
                memcpy(&can_long_frame_buf->can_rcv_buf[buf_index].rcv_buf[0], rx_buf.can_data_t.Data, CAN_ONE_FRAME_DATA_LENTH);
                can_long_frame_buf->can_rcv_buf[buf_index].used_len = CAN_ONE_FRAME_DATA_LENTH;
                can_long_frame_buf->can_rcv_buf[buf_index].can_id = id.canx_id;
                can_long_frame_buf->can_rcv_buf[buf_index].start_time = os_get_time();
                can_protocol_log("begin\r\n");
            }
            else if((seg_polo == TRANSING) || (seg_polo == END))
            {
                buf_index = can_long_frame_buf->get_the_buf_by_id(id.canx_id);
                if((buf_index == CAN_BUF_NO_THIS_ID) || (buf_index >= CAN_LONG_BUF_NUM))
                {
                    can_protocol_log("ERROR ! !\r\n long buff index is %d",buf_index);
                    goto exit;
                }
                can_long_frame_buf->can_rcv_buf[buf_index].start_time = os_get_time();
                if(seg_polo == TRANSING)
                {
                    memcpy(&can_long_frame_buf->can_rcv_buf[buf_index].rcv_buf[seg_num*CAN_ONE_FRAME_DATA_LENTH], rx_buf.can_data_t.Data, CAN_ONE_FRAME_DATA_LENTH);
                    can_long_frame_buf->can_rcv_buf[buf_index].used_len += CAN_ONE_FRAME_DATA_LENTH;
                    can_protocol_log("transing\r\n");
                }
                if(seg_polo == END)
                {
                    memcpy(&can_long_frame_buf->can_rcv_buf[buf_index].rcv_buf[seg_num*CAN_ONE_FRAME_DATA_LENTH], rx_buf.can_data_t.Data, rx_data_len - 1);
                    can_long_frame_buf->can_rcv_buf[buf_index].used_len += rx_data_len - 1;

                    //process the data here//
                    /**********************/
                    //process the data here//

                    tx_can_data( MICO_CAN1, id.canx_id, can_long_frame_buf->can_rcv_buf[buf_index].rcv_buf, can_long_frame_buf->can_rcv_buf[buf_index].used_len);  // test :send the data back;
                    can_long_frame_buf->free_buf(buf_index);
                    can_protocol_log("end\r\n");
                }
            }
        }
    }

exit:
    return;
}
