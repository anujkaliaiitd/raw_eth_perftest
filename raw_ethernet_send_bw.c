#if defined(__FreeBSD__)
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/types.h>
#endif

#include </usr/include/netinet/ip.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multicast_resources.h"
#include "perftest_communication.h"
#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "raw_ethernet_resources.h"

/******************************************************************************
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {
  struct ibv_device *ib_dev = NULL;
  struct pingpong_context ctx;
  struct raw_ethernet_info *my_dest_info = NULL;
  struct raw_ethernet_info *rem_dest_info = NULL;
  int ret_parser;
  struct perftest_parameters user_param;

  struct ibv_exp_flow **flow_create_result;
  struct ibv_exp_flow_attr **flow_rules;
  struct ibv_exp_flow **flow_promisc = NULL;
  struct ibv_exp_flow **flow_sniffer = NULL;
  int flow_index, qp_index;

  /* init default values to user's parameters */
  memset(&ctx, 0, sizeof(struct pingpong_context));
  memset(&user_param, 0, sizeof(struct perftest_parameters));

  user_param.verb = SEND;
  user_param.tst = BW;
  strncpy(user_param.version, VERSION, sizeof(user_param.version));
  user_param.connection_type = RawEth;

  ret_parser = parser(&user_param, argv, argc);

  if (ret_parser) {
    if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT) {
      fprintf(stderr, " Parser function exited with Error\n");
    }
    DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
    return FAILURE;
  }

  /* Allocate user input dependable structs */
  ALLOCATE(my_dest_info, struct raw_ethernet_info, user_param.num_of_qps);
  memset(my_dest_info, 0,
         sizeof(struct raw_ethernet_info) * user_param.num_of_qps);
  ALLOCATE(rem_dest_info, struct raw_ethernet_info, user_param.num_of_qps);
  memset(rem_dest_info, 0,
         sizeof(struct raw_ethernet_info) * user_param.num_of_qps);

  ALLOCATE(flow_create_result, struct ibv_exp_flow *,
           user_param.flows * user_param.num_of_qps);
  ALLOCATE(flow_rules, struct ibv_exp_flow_attr *,
           user_param.flows * user_param.num_of_qps);
  ALLOCATE(flow_sniffer, struct ibv_exp_flow *, user_param.num_of_qps);
  ALLOCATE(flow_promisc, struct ibv_exp_flow *, user_param.num_of_qps);

  /* Finding the IB device selected (or default if no selected). */
  ib_dev = ctx_find_dev(user_param.ib_devname);
  if (!ib_dev) {
    fprintf(stderr, " Unable to find the Infiniband/RoCE device\n");
    DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
    return FAILURE;
  }
  GET_STRING(user_param.ib_devname, ibv_get_device_name(ib_dev));

  if (check_flow_steering_support(user_param.ib_devname)) {
    return FAILURE;
  }

  /* Getting the relevant context from the device */
  ctx.context = ibv_open_device(ib_dev);
  if (!ctx.context) {
    fprintf(stderr, " Couldn't get context for the device\n");
    DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
    return FAILURE;
  }

  /* Verify user parameters that require the device context,
   * the function will print the relevent error info. */
  if (verify_params_with_device_context(ctx.context, &user_param)) {
    return FAILURE;
  }

  /* See if MTU and link type are valid and supported. */
  if (check_link_and_mtu(ctx.context, &user_param)) {
    fprintf(stderr, " Couldn't get context for the device\n");
    DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
    return FAILURE;
  }

  /* Allocating arrays needed for the test. */
  alloc_ctx(&ctx, &user_param);

  /* set mac address by user choose */
  for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
    if (send_set_up_connection(&flow_rules[qp_index * user_param.flows], &ctx,
                               &user_param, &my_dest_info[qp_index],
                               &rem_dest_info[qp_index])) {
      fprintf(stderr, " Unable to set up socket connection\n");
      return FAILURE;
    }
  }

  /* Print basic test information. */
  ctx_print_test_info(&user_param);

  if (!user_param.raw_mcast &&
      (user_param.machine == SERVER || user_param.duplex)) {
    for (flow_index = 0; flow_index < user_param.flows; flow_index++)
      print_spec(flow_rules[flow_index], &user_param);
  }

  /* create all the basic IB resources (data buffer, PD, MR, CQ and events
   * channel) */
  if (ctx_init(&ctx, &user_param)) {
    fprintf(stderr, " Couldn't create IB resources\n");
    return FAILURE;
  }

  /* build raw Ethernet packets on ctx buffer */
  if ((user_param.machine == CLIENT || user_param.duplex) &&
      !user_param.mac_fwd) {
    for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
      create_raw_eth_pkt(&user_param, &ctx, (void *)ctx.buf[qp_index],
                         &my_dest_info[qp_index], &rem_dest_info[qp_index]);
      printf("Packet bytes:\n");
      for (int i = 0; i < 60; i++) {
        if (i >= 14) ((uint8_t *)ctx.buf[qp_index])[i] = 0;
        printf("%02x ", ((uint8_t *)ctx.buf[qp_index])[i]);
      }
      printf("\n");
    }
  }

  /* create flow rules for servers/duplex clients ,  that not test raw_mcast */
  if (!user_param.raw_mcast &&
      (user_param.machine == SERVER || user_param.duplex)) {
    /* attaching the qp to the spec */
    for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
      for (flow_index = 0; flow_index < user_param.flows; flow_index++) {
        flow_create_result[flow_index + qp_index * user_param.flows] =
            ibv_exp_create_flow(ctx.qp[qp_index], flow_rules[flow_index]);

        if (!flow_create_result[flow_index + qp_index * user_param.flows]) {
          perror("error");
          fprintf(stderr, "Couldn't attach QP\n");
          return FAILURE;
        }
      }
    }

    if (user_param.use_promiscuous) {
      struct ibv_exp_flow_attr attr = {.type = IBV_EXP_FLOW_ATTR_ALL_DEFAULT,
                                       .num_of_specs = 0,
                                       .port = user_param.ib_port,
                                       .flags = 0};

      for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
        if ((flow_promisc[qp_index] =
                 ibv_exp_create_flow(ctx.qp[qp_index], &attr)) == NULL) {
          perror("error");
          fprintf(stderr, "Couldn't attach promiscuous rule QP\n");
        }
      }
    }
  }

  /* Prepare IB resources for rtr/rts. */
  if (ctx_connect(&ctx, NULL, &user_param, NULL)) {
    fprintf(stderr, " Unable to Connect the HCA's through the link\n");
    DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
    return FAILURE;
  }

  if (user_param.output == FULL_VERBOSITY) {
    printf(RESULT_LINE);
    if (user_param.raw_qos)
      printf(
          (user_param.report_fmt == MBS ? RESULT_FMT_QOS : RESULT_FMT_G_QOS));
    else
      printf((user_param.report_fmt == MBS ? RESULT_FMT : RESULT_FMT_G));
    printf(
        (user_param.cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
  }

  if (user_param.test_method == RUN_REGULAR) {
    if (user_param.machine == CLIENT || user_param.duplex) {
      ctx_set_send_wqes(&ctx, &user_param, NULL);
    }

    if (user_param.machine == SERVER || user_param.duplex) {
      if (ctx_set_recv_wqes(&ctx, &user_param)) {
        fprintf(stderr, " Failed to post receive recv_wqes\n");
        DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
        return FAILURE;
      }
    }

    if (user_param.mac_fwd) {
      if (run_iter_fw(&ctx, &user_param)) {
        DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
        return FAILURE;
      }

    } else if (user_param.duplex) {
      if (run_iter_bi(&ctx, &user_param)) {
        DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
        return FAILURE;
      }

    } else if (user_param.machine == CLIENT) {
      if (run_iter_bw(&ctx, &user_param)) {
        DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
        return FAILURE;
      }

    } else {
      if (run_iter_bw_server(&ctx, &user_param)) {
        DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
        return FAILURE;
      }
    }

    print_report_bw(&user_param, NULL);
  }

  if (user_param.machine == SERVER || user_param.duplex) {
    /* destroy open flows */
    for (flow_index = 0; flow_index < user_param.flows; flow_index++) {
      for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
        if (ibv_exp_destroy_flow(
                flow_create_result[flow_index + qp_index * user_param.flows])) {
          perror("error");
          fprintf(stderr, "Couldn't destroy flow\n");
          return FAILURE;
        }
      }
    }
    free(flow_rules);

    if (user_param.use_promiscuous) {
      for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
        if (ibv_exp_destroy_flow(flow_promisc[qp_index])) {
          perror("error");
          fprintf(stderr, "Couldn't destroy flow\n");
          return FAILURE;
        }
      }
      free(flow_promisc);
    }
  }

  if (destroy_ctx(&ctx, &user_param)) {
    fprintf(stderr, "Failed to destroy_ctx\n");
    DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
    return FAILURE;
  }

  free(my_dest_info);
  free(rem_dest_info);

  /* limit verifier */
  if (!user_param.is_bw_limit_passed && (user_param.is_limit_bw == ON)) {
    fprintf(stderr, "Error: BW result is below bw limit\n");
    return FAILURE;
  }

  if (user_param.output == FULL_VERBOSITY) printf(RESULT_LINE);

  DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
  return SUCCESS;
}
