/**
  ******************************************************************************
  * @file    hci.c
  * @author  AMG RF Application Team
  * @brief   Function for managing framework required for handling HCI interface.
  ******************************************************************************
  *
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
  *
  * <h2><center>&copy; COPYRIGHT 2013 STMicroelectronics</center></h2>
  */

#ifdef __cplusplus
 extern "C" {
#endif

#include "hal_types.h"
#include "osal.h"
#include "ble_status.h"
#include "hal.h"
#include "hci_const.h"
#include "gp_timer.h"
#include "bluenrg_interface.h"
#include "ble_list.h"
#include "thread.h"
#include "cond.h"
#include "mutex.h"

#include "stm32_bluenrg_ble.h"

#if BLE_CONFIG_DBG_ENABLE
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

#define HCI_LOG_ON 0

#define HCI_READ_PACKET_NUM_MAX 		 (5)

#define MIN(a,b)            ((a) < (b) )? (a) : (b)
#define MAX(a,b)            ((a) > (b) )? (a) : (b)

tListNode hciReadPktPool;
tListNode hciReadPktRxQueue;
/* pool of hci read packets */
static tHciDataPacket     hciReadPacketBuffer[HCI_READ_PACKET_NUM_MAX];

static volatile uint8_t hci_timer_id;
static volatile uint8_t hci_timeout;

void hci_timeout_callback(void)
{
  hci_timeout = 1;
  return;
}

char hci_thread_stack[THREAD_STACKSIZE_MAIN];
void* HCI_Reader_Thread(void *arg);

static kernel_pid_t thread_pid = -1;

void HCI_Init(void)
{
  uint8_t index;

  /* Initialize list heads of ready and free hci data packet queues */
  ble_list_init_head (&hciReadPktPool);
  ble_list_init_head (&hciReadPktRxQueue);

  /* Initialize the queue of free hci data packets */
  for (index = 0; index < HCI_READ_PACKET_NUM_MAX; index++)
  {
    ble_list_insert_tail(&hciReadPktPool, (tListNode *)&hciReadPacketBuffer[index]);
  }

  if(thread_pid == -1) {
    thread_pid = thread_create(hci_thread_stack, sizeof(hci_thread_stack),
      THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST,
      HCI_Reader_Thread, NULL, "hci_reader_thread");
  }
}

#define HCI_PCK_TYPE_OFFSET                 0
#define  EVENT_PARAMETER_TOT_LEN_OFFSET     2

/**
 * Verify if HCI packet is correctly formatted.
 *
 * @param[in] hciReadPacket    The packet that is received from HCI interface.
 * @return 0 if HCI packet is as expected
 */
int HCI_verify(const tHciDataPacket * hciReadPacket)
{
  const uint8_t *hci_pckt = hciReadPacket->dataBuff;

  if(hci_pckt[HCI_PCK_TYPE_OFFSET] != HCI_EVENT_PKT)
    return 1;  /* Incorrect type. */

  if(hci_pckt[EVENT_PARAMETER_TOT_LEN_OFFSET] != hciReadPacket->data_len - (1+HCI_EVENT_HDR_SIZE))
    return 2; /* Wrong length (packet truncated or too long). */

  return 0;
}

void HCI_Process(void)
{
  tHciDataPacket * hciReadPacket = NULL;

  Disable_SPI_IRQ();
  uint8_t list_empty = ble_list_is_empty(&hciReadPktRxQueue);
  /* process any pending events read */
  while(list_empty == FALSE)
  {
    ble_list_remove_head (&hciReadPktRxQueue, (tListNode **)&hciReadPacket);
    Enable_SPI_IRQ();
    HCI_Event_CB(hciReadPacket->dataBuff);
    Disable_SPI_IRQ();
    ble_list_insert_tail(&hciReadPktPool, (tListNode *)hciReadPacket);
    list_empty = ble_list_is_empty(&hciReadPktRxQueue);
  }
  /* Explicit call to HCI_Isr(), since it cannot be called by ISR if IRQ is kept high by
  BlueNRG. */
  HCI_Isr();
  Enable_SPI_IRQ();
}

BOOL HCI_Queue_Empty(void)
{
  return ble_list_is_empty(&hciReadPktRxQueue);
}

static cond_t hci_reader_cond = COND_INIT;
static mutex_t hci_reader_mutex = MUTEX_INIT;

void HCI_Isr(void)
{
  // We can't use SPI from inside of an interrupt handler, signal the reader thread
  // that data is available to be read

  cond_signal(&hci_reader_cond);
}

void* HCI_Reader_Thread(void *arg)
{
  (void)arg;

  tHciDataPacket * hciReadPacket = NULL;
  uint8_t data_len;

  while(1) {

    // We don't strictly need a mutex here, as there is only a single thread waiting
    // on the condition variable. Nevertheless, cond_wait expects a mutex
    mutex_lock(&hci_reader_mutex);
    cond_wait(&hci_reader_cond, &hci_reader_mutex);

    while(BlueNRG_DataPresent()){
      if (ble_list_is_empty (&hciReadPktPool) == FALSE){

        /* enqueueing a packet for read */
        ble_list_remove_head (&hciReadPktPool, (tListNode **)&hciReadPacket);

        data_len = BlueNRG_SPI_Read_All(hciReadPacket->dataBuff, HCI_READ_PACKET_SIZE);
        if(data_len > 0){
          hciReadPacket->data_len = data_len;
          if(HCI_verify(hciReadPacket) == 0)
            ble_list_insert_tail(&hciReadPktRxQueue, (tListNode *)hciReadPacket);
          else
            ble_list_insert_head(&hciReadPktPool, (tListNode *)hciReadPacket);
        }
        else {
          // Insert the packet back into the pool.
          ble_list_insert_head(&hciReadPktPool, (tListNode *)hciReadPacket);
        }

      }
      else {
        // HCI Read Packet Pool is empty, wait for a free packet.
        break;
      }
    }

    mutex_unlock(&hci_reader_mutex);
  }
}

void hci_write(const void* data1, const void* data2, uint8_t n_bytes1, uint8_t n_bytes2){
#if  HCI_LOG_ON
  PRINTF("HCI <- ");
  for(int i=0; i < n_bytes1; i++)
    PRINTF("%02X ", *((uint8_t*)data1 + i));
  for(int i=0; i < n_bytes2; i++)
    PRINTF("%02X ", *((uint8_t*)data2 + i));
  PRINTF("\n");
#endif

  Hal_Write_Serial(data1, data2, n_bytes1, n_bytes2);
}

void hci_send_cmd(uint16_t ogf, uint16_t ocf, uint8_t plen, void *param)
{
  hci_command_hdr hc;

  hc.opcode = htobs(cmd_opcode_pack(ogf, ocf));
  hc.plen= plen;

  uint8_t header[HCI_HDR_SIZE + HCI_COMMAND_HDR_SIZE];
  header[0] = HCI_COMMAND_PKT;
  Osal_MemCpy(header+1, &hc, sizeof(hc));

  hci_write(header, param, sizeof(header), plen);
}

static void move_list(tListNode * dest_list, tListNode * src_list)
{
  pListNode tmp_node;

  while(!ble_list_is_empty(src_list)){
    ble_list_remove_tail(src_list, &tmp_node);
    ble_list_insert_head(dest_list, tmp_node);
  }
}

 /* It ensures that we have at least half of the free buffers in the pool. */
static void free_event_list(void)
{
  tHciDataPacket * pckt;

  Disable_SPI_IRQ();

  while(ble_list_get_size(&hciReadPktPool) < HCI_READ_PACKET_NUM_MAX/2){
    ble_list_remove_head(&hciReadPktRxQueue, (tListNode **)&pckt);
    ble_list_insert_tail(&hciReadPktPool, (tListNode *)pckt);
    /* Explicit call to HCI_Isr(), since it cannot be called by ISR if IRQ is kept high by
    BlueNRG */
    HCI_Isr();
  }

  Enable_SPI_IRQ();
}

int hci_send_req(struct hci_request *r, BOOL async)
{
  uint8_t *ptr;
  uint16_t opcode = htobs(cmd_opcode_pack(r->ogf, r->ocf));
  hci_event_pckt *event_pckt;
  hci_uart_pckt *hci_hdr;
  int to = DEFAULT_TIMEOUT;
  struct timer t;
  tHciDataPacket * hciReadPacket = NULL;
  tListNode hciTempQueue;

  ble_list_init_head(&hciTempQueue);

  free_event_list();

  hci_send_cmd(r->ogf, r->ocf, r->clen, r->cparam);

  if(async){
    return 0;
  }

  /* Minimum timeout is 1. */
  if(to == 0)
    to = 1;

  Timer_Set(&t, to);

  while(1) {
    evt_cmd_complete *cc;
    evt_cmd_status *cs;
    evt_le_meta_event *me;
    int len;

    while(1){
      if(Timer_Expired(&t)){
        goto failed;
      }
      if(!HCI_Queue_Empty()){
        break;
      }
    }

    /* Extract packet from HCI event queue. */
    Disable_SPI_IRQ();
    ble_list_remove_head(&hciReadPktRxQueue, (tListNode **)&hciReadPacket);

    hci_hdr = (void *)hciReadPacket->dataBuff;

    if(hci_hdr->type == HCI_EVENT_PKT){

    event_pckt = (void *) (hci_hdr->data);

    ptr = hciReadPacket->dataBuff + (1 + HCI_EVENT_HDR_SIZE);
    len = hciReadPacket->data_len - (1 + HCI_EVENT_HDR_SIZE);

    switch (event_pckt->evt) {

    case EVT_CMD_STATUS:
      cs = (void *) ptr;

      if (cs->opcode != opcode)
        goto failed;

      if (r->event != EVT_CMD_STATUS) {
        if (cs->status) {
          goto failed;
        }
        break;
      }

      r->rlen = MIN(len, r->rlen);
      Osal_MemCpy(r->rparam, ptr, r->rlen);
      goto done;

    case EVT_CMD_COMPLETE:
      cc = (void *) ptr;

      if (cc->opcode != opcode)
        goto failed;

      ptr += EVT_CMD_COMPLETE_SIZE;
      len -= EVT_CMD_COMPLETE_SIZE;

      r->rlen = MIN(len, r->rlen);
      Osal_MemCpy(r->rparam, ptr, r->rlen);
      goto done;

    case EVT_LE_META_EVENT:
      me = (void *) ptr;

      if (me->subevent != r->event)
        break;

      len -= 1;
      r->rlen = MIN(len, r->rlen);
      Osal_MemCpy(r->rparam, me->data, r->rlen);
      goto done;

    case EVT_HARDWARE_ERROR:
      goto failed;

    default:
      break;
      }
    }

    /* If there are no more packets to be processed, be sure there is at list one
       packet in the pool to process the expected event.
       If no free packets are available, discard the processed event and insert it
       into the pool. */
    if(ble_list_is_empty(&hciReadPktPool) && ble_list_is_empty(&hciReadPktRxQueue)){
      ble_list_insert_tail(&hciReadPktPool, (tListNode *)hciReadPacket);
      hciReadPacket=NULL;
    }
    else {
      /* Insert the packet in a different queue. These packets will be
      inserted back in the main queue just before exiting from send_req(), so that
      these events can be processed by the application.
    */
    ble_list_insert_tail(&hciTempQueue, (tListNode *)hciReadPacket);
      hciReadPacket=NULL;
    }

    HCI_Isr();

    Enable_SPI_IRQ();

  }

failed:
  if(hciReadPacket!=NULL){
    ble_list_insert_head(&hciReadPktPool, (tListNode *)hciReadPacket);
  }
  move_list(&hciReadPktRxQueue, &hciTempQueue);
  Enable_SPI_IRQ();
  return -1;

done:
  // Insert the packet back into the pool.
  ble_list_insert_head(&hciReadPktPool, (tListNode *)hciReadPacket);
  move_list(&hciReadPktRxQueue, &hciTempQueue);

  Enable_SPI_IRQ();
  return 0;
}

#ifdef __cplusplus
 }
#endif
