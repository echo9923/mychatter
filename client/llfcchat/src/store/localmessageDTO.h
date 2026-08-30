#ifndef LOCALMESSAGEDTO_H
#define LOCALMESSAGEDTO_H

#include <QList>
#include <QMetaType>
#include <QString>

struct UserEventDTO {
	qint64 event_seq = 0;
	int event_type = 0;
	qint64 message_id = 0;
	qint64 friend_request_id = 0;
};

struct LocalMessageDTO {
	qint64 local_message_id = 0;
	qint64 message_id = 0;
	QString client_message_id;
	qint64 thread_id = 0;
	qint64 sender_user_id = 0;
	int message_type = 0;
	QString text_content;
	int send_status = 0;
	qint64 created_at = 0;
};

struct LocalMessageResourceDTO {
	qint64 local_message_id = 0;
	QString original_file_name;
	QString local_file_path;
	qint64 file_size_bytes = 0;
	QString sha256;
	QString mime_type;
};

struct LocalConversationDTO {
	qint64 thread_id = 0;
	qint64 peer_user_id = 0;
	qint64 last_message_id = 0;
	QString last_message_preview;
	int unread_count = 0;
	qint64 oldest_loaded_message_id = 0;
	bool history_complete = false;
	qint64 updated_at = 0;
};

struct LocalFriendRequestDTO {
	qint64 friend_request_id = 0;
	qint64 requester_user_id = 0;
	qint64 target_user_id = 0;
	QString request_message;
	int status = 0;
	qint64 thread_id = 0;
	QString peer_username;
	QString peer_nickname;
	QString peer_avatar_key;
	int peer_gender = 0;
};

struct LocalContactDTO {
	qint64 user_id = 0;
	qint64 thread_id = 0;
	QString username;
	QString nickname;
	QString avatar_key;
	int gender = 0;
};

struct OutboxEntryDTO {
	qint64 operation_id = 0;
	QString client_message_id;
	QString operation_type;
	QString stage;
	QString payload_json;
};

Q_DECLARE_METATYPE(UserEventDTO)
Q_DECLARE_METATYPE(LocalMessageDTO)
Q_DECLARE_METATYPE(LocalMessageResourceDTO)
Q_DECLARE_METATYPE(LocalConversationDTO)
Q_DECLARE_METATYPE(LocalFriendRequestDTO)
Q_DECLARE_METATYPE(LocalContactDTO)
Q_DECLARE_METATYPE(OutboxEntryDTO)
Q_DECLARE_METATYPE(QList<UserEventDTO>)
Q_DECLARE_METATYPE(QList<LocalMessageDTO>)
Q_DECLARE_METATYPE(QList<LocalMessageResourceDTO>)
Q_DECLARE_METATYPE(QList<LocalConversationDTO>)
Q_DECLARE_METATYPE(QList<LocalFriendRequestDTO>)
Q_DECLARE_METATYPE(QList<LocalContactDTO>)
Q_DECLARE_METATYPE(QList<OutboxEntryDTO>)

#endif
