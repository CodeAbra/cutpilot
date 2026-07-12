#pragma once

#include "cutpilot/core/Connection.h"
#include "cutpilot/core/command/Command.h"

namespace cutpilot::core {

// Adds a connection from an output port to an input port. An input holds at most
// one edge, so an edge already feeding the target input is removed with the add and
// restored on revert. The first apply assigns and remembers the connection's id; a
// later apply (redo) re-creates it identically.
class ConnectCommand : public Command {
public:
    explicit ConnectCommand(const Connection &connection);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

    int connectionId() const { return m_connection.id; }

private:
    Connection m_connection;
    Connection m_replaced;
    int m_replacedIndex = -1;
    int m_index = 0;
    bool m_captured = false;
};

} // namespace cutpilot::core
