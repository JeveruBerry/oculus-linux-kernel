/* Copyright (c) 2019, Facebook Technologies, LLC. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include "msm_vidc_internal.h"
#include "vidc_hfi.h"
#include "vidc_threadstats.h"

static void vidc_destroy_thread_private(struct kref *kref)
{
	struct vidc_thread_private *private = container_of(kref,
			struct vidc_thread_private, refcount);

	kfree(private);
}

int vidc_thread_private_get(struct vidc_thread_private *thread)
{
	int ret = 0;

	if (thread != NULL)
		ret = kref_get_unless_zero(&thread->refcount);
	return ret;
}

void vidc_thread_private_put(struct vidc_thread_private *thread)
{
	if (thread)
		kref_put(&thread->refcount, vidc_destroy_thread_private);
}

struct vidc_thread_private *vidc_thread_private_find(pid_t tid)
{
	struct vidc_thread_private *t, *private = NULL;

	mutex_lock(&vidc_driver->thread_mutex);
	list_for_each_entry(t, &vidc_driver->thread_list, list) {
		if (t->tid == tid) {
			if (vidc_thread_private_get(t))
				private = t;
			break;
		}
	}
	mutex_unlock(&vidc_driver->thread_mutex);
	return private;
}

static struct vidc_thread_private *vidc_thread_private_new(void)
{
	struct vidc_thread_private *private;
	pid_t tid = task_pid_nr(current);

	list_for_each_entry(private, &vidc_driver->thread_list, list) {
		if (private->tid == tid) {
			if (!vidc_thread_private_get(private))
				private = ERR_PTR(-EINVAL);
			return private;
		}
	}

	private = kzalloc(sizeof(struct vidc_thread_private), GFP_KERNEL);
	if (private == NULL)
		return ERR_PTR(-ENOMEM);

	kref_init(&private->refcount);

	private->tid = tid;
	get_task_comm(private->comm, current);

	return private;
}

struct vidc_threadstat_attribute {
	struct attribute attr;
	int timestamp_type;
	int id_type;
	int count_type;
	ssize_t (*show)(struct vidc_thread_private *priv, int timestamp_type,
		int id_type, int count_type, char *buf);
};

static ssize_t
vidc_threadstat_multiattr_show(
	struct vidc_thread_private *priv,
	int timestamp_type,
	int id_type,
	int count_type,
	char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%llu,%llu,%llu\n",
		priv->stats[timestamp_type],
		priv->stats[id_type],
		priv->stats[count_type]);
}

#define VIDC_THREADSTAT_MULTIATTR(_type, _name) \
[_type ## _EVENT] = { \
	.attr = { .name = __stringify(_name), .mode = 0444 }, \
	.timestamp_type = _type ## _TIMESTAMP, \
	.id_type = _type ## _ID, \
	.count_type = _type ## _COUNT, \
	.show = vidc_threadstat_multiattr_show, \
}

struct vidc_threadstat_attribute vidc_threadstat_attrs[] = {
	VIDC_THREADSTAT_MULTIATTR(VIDC_THREADSTATS_ETB, etb),
	VIDC_THREADSTAT_MULTIATTR(VIDC_THREADSTATS_EBD, ebd),
	VIDC_THREADSTAT_MULTIATTR(VIDC_THREADSTATS_FTB, ftb),
	VIDC_THREADSTAT_MULTIATTR(VIDC_THREADSTATS_FBD, fbd),
};

#define to_vidc_threadstat_attr(a) \
container_of(a, struct vidc_threadstat_attribute, attr)

static ssize_t vidc_threadstat_sysfs_show(struct kobject *kobj,
	struct attribute *attr, char *buf)
{
	struct vidc_threadstat_attribute *pattr = to_vidc_threadstat_attr(attr);
	struct vidc_thread_private *priv;
	ssize_t ret;

	priv = kobj ? container_of(kobj, struct vidc_thread_private, kobj) :
			NULL;

	if (priv && pattr->show) {
		ret = pattr->show(
			priv,
			pattr->timestamp_type,
			pattr->id_type,
			pattr->count_type,
			buf);
	} else
		ret = -EIO;

	return ret;
}

static const struct sysfs_ops vidc_threadstat_sysfs_ops = {
	.show = vidc_threadstat_sysfs_show,
};

static struct kobj_type vidc_ktype_threadstat = {
	.sysfs_ops = &vidc_threadstat_sysfs_ops,
};

void vidc_thread_uninit_sysfs(struct vidc_thread_private *private)
{
	int i;

	for (i = 0; i < VIDC_THREADSTATS_EVENT_MAX; i++) {
		sysfs_put(private->event_sd[i]);
		sysfs_remove_file(&private->kobj,
			&vidc_threadstat_attrs[i].attr);
	}

	kobject_put(&private->kobj);

	/* Put the refcount we got in vidc_thread_init_sysfs */
	vidc_thread_private_put(private);
}

void vidc_thread_init_sysfs(struct vidc_thread_private *private)
{
	int i;
	unsigned char name[16];

	/* Keep private valid until the sysfs enries are removed. */
	vidc_thread_private_get(private);

	snprintf(name, sizeof(name), "%d", private->tid);

	if (kobject_init_and_add(&private->kobj, &vidc_ktype_threadstat,
		vidc_driver->threadkobj, name)) {
		WARN(1, "Unable to add sysfs dir '%s'\n", name);
		return;
	}

	for (i = 0; i < VIDC_THREADSTATS_MAX; i++)
		private->stats[i] = 0;

	for (i = 0; i < VIDC_THREADSTATS_EVENT_MAX; i++) {
		if (sysfs_create_file(&private->kobj,
			&vidc_threadstat_attrs[i].attr))
			WARN(1, "Couldn't create sysfs file '%s'\n",
				vidc_threadstat_attrs[i].attr.name);
		private->event_sd[i] = sysfs_get_dirent(
			private->kobj.sd, vidc_threadstat_attrs[i].attr.name);
	}
}

void vidc_thread_private_close(struct vidc_thread_private *private)
{
	mutex_lock(&vidc_driver->thread_mutex);

	if (--private->fd_count > 0) {
		mutex_unlock(&vidc_driver->thread_mutex);
		vidc_thread_private_put(private);
		return;
	}

	pr_debug("thread: %s [%d]\n", private->comm, private->tid);
	vidc_thread_uninit_sysfs(private);
	list_del(&private->list);

	mutex_unlock(&vidc_driver->thread_mutex);
	vidc_thread_private_put(private);
}

struct vidc_thread_private *vidc_thread_private_open(void)
{
	struct vidc_thread_private *private;

	mutex_lock(&vidc_driver->thread_mutex);
	private = vidc_thread_private_new();

	if (IS_ERR(private))
		goto done;

	if (private->fd_count++ == 0) {
		pr_debug("thread: %s [%d]\n", private->comm, private->tid);
		vidc_thread_init_sysfs(private);
		list_add(&private->list, &vidc_driver->thread_list);
	}

done:
	mutex_unlock(&vidc_driver->thread_mutex);
	return private;
}
