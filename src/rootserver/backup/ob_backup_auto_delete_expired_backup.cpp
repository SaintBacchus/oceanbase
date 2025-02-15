/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX RS
#include "ob_backup_auto_delete_expired_backup.h"
#include "share/backup/ob_backup_operator.h"
#include "rootserver/ob_rs_event_history_table_operator.h"
#include "share/backup/ob_tenant_backup_clean_info_updater.h"
#include "rootserver/ob_backup_data_clean_scheduler.h"

namespace oceanbase {

using namespace common;
using namespace obrpc;
using namespace share;
using namespace share::schema;
using namespace storage;

namespace rootserver {

int64_t OBackupAutoDeleteExpiredIdling::get_idle_interval_us()
{
  int64_t auto_delete_check_interval = 5 * 60 * 1000 * 1000;  // 5min
#ifdef ERRSIM
  int ret = OB_SUCCESS;
  if (OB_SUCC(ret)) {
    ret = E(EventTable::EN_BACKUP_AUTO_DELETE_INTERVAL) OB_SUCCESS;
    auto_delete_check_interval = 10 * 1000 * 1000;  // 10s;
    ret = OB_SUCCESS;
  }
#endif
  return auto_delete_check_interval;
}

ObBackupAutoDeleteExpiredData::ObBackupAutoDeleteExpiredData()
    : is_inited_(false),
      config_(NULL),
      sql_proxy_(NULL),
      schema_service_(NULL),
      idling_(stop_),
      backup_data_clean_(NULL),
      is_working_(false),
      backup_lease_service_(nullptr),
      delete_obsolete_action_(ObBackupDeleteObsoleteAction::NONE)
{}

ObBackupAutoDeleteExpiredData::~ObBackupAutoDeleteExpiredData()
{}

int ObBackupAutoDeleteExpiredData::init(common::ObServerConfig &cfg, ObMySQLProxy &sql_proxy,
    share::schema::ObMultiVersionSchemaService &schema_service, ObBackupDataClean &backup_data_clean,
    share::ObIBackupLeaseService &backup_lease_service)
{
  int ret = OB_SUCCESS;
  const int backup_auto_delete_thread_cnt = 1;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    STORAGE_LOG(WARN, "root backup init twice", K(ret));
  } else if (OB_FAIL(create(backup_auto_delete_thread_cnt, "BackupAutoDelete"))) {
    LOG_WARN("create thread failed", K(ret), K(backup_auto_delete_thread_cnt));
  } else {
    config_ = &cfg;
    sql_proxy_ = &sql_proxy;
    schema_service_ = &schema_service;
    backup_data_clean_ = &backup_data_clean;
    backup_lease_service_ = &backup_lease_service;
    is_inited_ = true;
    LOG_INFO("backup auto delete expired data init success");
  }
  return ret;
}

int ObBackupAutoDeleteExpiredData::idle() const
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(idling_.idle())) {
    LOG_WARN("idle failed", K(ret));
  } else {
    LOG_INFO("backup auto delete expired data idle", "idle_time", idling_.get_idle_interval_us());
  }
  return ret;
}

void ObBackupAutoDeleteExpiredData::wakeup()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    idling_.wakeup();
  }
}

void ObBackupAutoDeleteExpiredData::stop()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    ObRsReentrantThread::stop();
    idling_.wakeup();
  }
}

int ObBackupAutoDeleteExpiredData::start()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObReentrantThread::start())) {
    LOG_WARN("failed to start", K(ret));
  } else {
    is_working_ = true;
  }
  return ret;
}

void ObBackupAutoDeleteExpiredData::run3()
{
  int ret = OB_SUCCESS;
  ObCurTraceId::init(GCONF.self_addr_);
  while (!stop_) {
    ret = OB_SUCCESS;

    if (stop_) {
      ret = OB_RS_SHUTDOWN;
      LOG_WARN("rootservice shutdown", K(ret));
    } else if (ObBackupDeleteObsoleteAction::NONE == delete_obsolete_action_) {
      switch_delete_obsolete_action();
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(schedule_auto_delete_obsolete())) {
      LOG_WARN("failed to schedule auto delete obsolete", K(ret));
    }

    if (OB_FAIL(idle())) {
      LOG_WARN("idle failed", K(ret));
    } else {
      continue;
    }
  }
  is_working_ = false;
}

int ObBackupAutoDeleteExpiredData::check_can_auto_handle_backup(
    const bool is_auto, const int64_t backup_recovery_window, bool &can_auto_delete)
{
  int ret = OB_SUCCESS;
  can_auto_delete = true;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("auto delete expired data do not init", K(ret));
  } else if (OB_FAIL(backup_lease_service_->get_lease_status(can_auto_delete))) {
    LOG_WARN("failed to check can backup", K(ret));
  } else if (can_auto_delete) {
    can_auto_delete = (is_auto && backup_recovery_window > 0);
  }
  return ret;
}

int ObBackupAutoDeleteExpiredData::get_last_succeed_delete_obsolete_snapshot(
    int64_t &last_succ_delete_obsolete_snapshot)
{
  int ret = OB_SUCCESS;
  ObBackupInfoManager backup_info_manager;
  last_succ_delete_obsolete_snapshot = 0;
  ObMySQLTransaction trans;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("backup auto delete expired data do not init", K(ret));
  } else if (ObBackupDeleteObsoleteAction::NONE == delete_obsolete_action_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("delete obsolete action is none, cannot get last succed delete obsolete snapshot",
        K(ret),
        K(delete_obsolete_action_));
  } else if (OB_FAIL(backup_info_manager.init(OB_SYS_TENANT_ID, *sql_proxy_))) {
    LOG_WARN("failed to init backup info manager", K(ret));
  } else if (OB_FAIL(trans.start(sql_proxy_))) {
    LOG_WARN("failed to start trans", K(ret));
  } else {
    const ObBackupManageArg::Type type =
        ObBackupDeleteObsoleteAction::DELETE_OBSOLETE_BACKUP_BACKUP == delete_obsolete_action_
            ? ObBackupManageArg::DELETE_OBSOLETE_BACKUP_BACKUP
            : ObBackupManageArg::DELETE_OBSOLETE_BACKUP;
    if (OB_FAIL(backup_info_manager.get_delete_obsolete_snapshot(
            OB_SYS_TENANT_ID, type, trans, last_succ_delete_obsolete_snapshot))) {
      LOG_WARN("failed to get last delete obsolete data snapshot", K(ret), K(type));
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(trans.end(true /*commit*/))) {
        OB_LOG(WARN, "failed to commit", K(ret));
      }
    } else {
      int tmp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp_ret = trans.end(false /* commit*/))) {
        OB_LOG(WARN, "failed to rollback trans", K(tmp_ret));
      }
    }
  }
  return ret;
}

int ObBackupAutoDeleteExpiredData::schedule_auto_delete_expired_data(const int64_t backup_recovery_window)
{
  int ret = OB_SUCCESS;
  int64_t last_succ_delete_obsolete_snapshot = 0;
  const int64_t now_ts = ObTimeUtil::current_time();
  ObBackupDataCleanScheduler backup_data_clean_scheduler;
  const int64_t MAX_INTERVAL = 24L * 60L * 60L * 1000L * 1000L;  // 24h
  const int64_t AUTO_CLEAN_INTERVAL =
      (backup_recovery_window / 2) < MAX_INTERVAL ? (backup_recovery_window / 2) : MAX_INTERVAL;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("backup auto delete expired data do not init", K(ret));
  } else if (backup_recovery_window <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("schedule auto delete expired data get invalid argument", K(ret), K(backup_recovery_window));
  } else if (OB_FAIL(get_last_succeed_delete_obsolete_snapshot(last_succ_delete_obsolete_snapshot))) {
    LOG_WARN("failed to get last succ delete obsolete snapshot", K(ret), K(last_succ_delete_obsolete_snapshot));
  } else if (now_ts - last_succ_delete_obsolete_snapshot < AUTO_CLEAN_INTERVAL) {
    switch_delete_obsolete_action();
    if (delete_obsolete_action_ != ObBackupDeleteObsoleteAction::NONE) {
      wakeup();
    }
  } else {
    obrpc::ObBackupManageArg arg;
    arg.tenant_id_ = OB_SYS_TENANT_ID;
    arg.type_ = ObBackupDeleteObsoleteAction::DELETE_OBSOLETE_BACKUP_BACKUP == delete_obsolete_action_
                    ? ObBackupManageArg::DELETE_OBSOLETE_BACKUP_BACKUP
                    : ObBackupManageArg::DELETE_OBSOLETE_BACKUP;
    arg.value_ = now_ts - backup_recovery_window;
    if (OB_FAIL(backup_data_clean_scheduler.init(arg, *schema_service_, *sql_proxy_, backup_data_clean_))) {
      LOG_WARN("failed to init backup data clean scheduler", K(ret), K(arg));
    } else if (OB_FAIL(backup_data_clean_scheduler.start_schedule_backup_data_clean())) {
      if (OB_BACKUP_DELETE_DATA_IN_PROGRESS == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("failed to schedule backup data clean", K(ret), K(arg));
      }
    } else {
      switch_delete_obsolete_action();
    }
  }
  return ret;
}

int ObBackupAutoDeleteExpiredData::schedule_auto_delete_obsolete()
{
  int ret = OB_SUCCESS;
  ObBackupDestOpt backup_dest_option;
  bool is_backup_backup = false;
  bool is_auto = true;
  int64_t backup_recovery_window = 0;
  bool can_auto_delete = true;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("backup auto delete expired data do not init", K(ret));
  } else if (ObBackupDeleteObsoleteAction::NONE == delete_obsolete_action_) {
    // do nothing
  } else if (FALSE_IT(is_backup_backup =
                          delete_obsolete_action_ == ObBackupDeleteObsoleteAction::DELETE_OBSOLETE_BACKUP_BACKUP
                              ? true
                              : false)) {
  } else if (OB_FAIL(backup_dest_option.init(is_backup_backup))) {
    LOG_WARN("failed to init backup dest option", K(ret));
  } else {
    is_auto = backup_dest_option.auto_delete_obsolete_backup_ || backup_dest_option.auto_touch_reserved_backup_;
    backup_recovery_window = backup_dest_option.recovery_window_;

#ifdef ERRSIM
    if (OB_SUCC(ret)) {
      ret = E(EventTable::EN_BACKUP_RECOVERY_WINDOW) OB_SUCCESS;
      if (OB_FAIL(ret)) {
        backup_recovery_window = 10 * 1000 * 1000L;  // 10s
        ret = OB_SUCCESS;
      }
    }
#endif

    if (OB_FAIL(check_can_auto_handle_backup(is_auto, backup_recovery_window, can_auto_delete))) {
      LOG_WARN("failed to check can backup", K(ret));
    } else if (!can_auto_delete) {
      // do nothing
      switch_delete_obsolete_action();
      if (delete_obsolete_action_ != ObBackupDeleteObsoleteAction::NONE) {
        wakeup();
      }
    } else if (OB_FAIL(schedule_auto_delete_expired_data(backup_recovery_window))) {
      LOG_WARN("failed to schedule auto delete expired data", K(ret), K(backup_recovery_window));
    }
  }
  return ret;
}

void ObBackupAutoDeleteExpiredData::switch_delete_obsolete_action()
{
  if (ObBackupDeleteObsoleteAction::NONE == delete_obsolete_action_) {
    delete_obsolete_action_ = ObBackupDeleteObsoleteAction::DELETE_OBSOLETE_BACKUP;
  } else if (ObBackupDeleteObsoleteAction::DELETE_OBSOLETE_BACKUP == delete_obsolete_action_) {
    delete_obsolete_action_ = ObBackupDeleteObsoleteAction::DELETE_OBSOLETE_BACKUP_BACKUP;
  } else if (ObBackupDeleteObsoleteAction::DELETE_OBSOLETE_BACKUP_BACKUP == delete_obsolete_action_) {
    delete_obsolete_action_ = ObBackupDeleteObsoleteAction::NONE;
  }
}

}  // namespace rootserver
}  // namespace oceanbase
