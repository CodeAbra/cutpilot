#include "cutpilot/core/CompositePlan.h"

#include "cutpilot/core/CompositeNodes.h"

#include <QCryptographicHash>
#include <QSet>

namespace cutpilot::core {

namespace {

// The wired inputs of a node, one entry per input port in port order.
QVector<CompositeInput> wiredInputs(const NodeGraph &graph, const Node &node)
{
    QVector<CompositeInput> inputs;
    for (int i = 0; i < node.ports.size(); ++i) {
        const Port &port = node.ports[i];
        if (!port.isInput)
            continue;
        CompositeInput input;
        const int connectionId = graph.connectionAtInput(node.id, i);
        const Connection *edge =
            connectionId != -1 ? graph.connectionById(connectionId) : nullptr;
        const Node *source = edge ? graph.nodeById(edge->fromNodeId) : nullptr;
        if (source && edge->fromPortIndex < source->ports.size()
            && producesImage(source->kind)) {
            input.nodeId = source->id;
            input.matte = source->ports[edge->fromPortIndex].type == PortType::Mask;
        }
        inputs.push_back(input);
    }
    return inputs;
}

// Post-order walk collecting sources and passes, dependencies first. Returns
// false when a cycle passes through the node.
bool collect(const NodeGraph &graph, int nodeId, QSet<int> &visiting,
             QSet<int> &done, CompositePlan &plan)
{
    if (done.contains(nodeId))
        return true;
    if (visiting.contains(nodeId))
        return false;

    const Node *node = graph.nodeById(nodeId);
    if (!node || !producesImage(node->kind))
        return true; // nothing to evaluate; the consumer sees it unwired

    if (!isCompositeKind(node->kind)) {
        done.insert(nodeId);
        plan.sourceNodeIds.push_back(nodeId);
        return true;
    }

    visiting.insert(nodeId);
    CompositePass pass;
    pass.nodeId = nodeId;
    pass.kind = node->kind;
    pass.params = node->comp;
    pass.inputs = wiredInputs(graph, *node);
    pass.signature = compositeSignature(graph, nodeId);
    for (const CompositeInput &input : pass.inputs) {
        if (input.nodeId != -1
            && !collect(graph, input.nodeId, visiting, done, plan))
            return false;
    }
    visiting.remove(nodeId);
    done.insert(nodeId);
    plan.passes.push_back(pass);
    return true;
}

void appendField(QByteArray &canonical, const QByteArray &field)
{
    canonical += QByteArray::number(field.size());
    canonical += ':';
    canonical += field;
}

QByteArray number(double value)
{
    return QByteArray::number(value, 'g', 17);
}

// The recursive canonical form behind compositeSignature. A cycle resolves to
// a marker so the signature stays total; the plan builder is what refuses
// cyclic graphs.
QByteArray canonicalForm(const NodeGraph &graph, int nodeId, QSet<int> &visiting)
{
    const Node *node = graph.nodeById(nodeId);
    if (!node || !producesImage(node->kind))
        return QByteArrayLiteral("none");
    if (visiting.contains(nodeId))
        return QByteArrayLiteral("cycle");

    QByteArray canonical;
    switch (node->kind) {
    case NodeKind::Still:
        appendField(canonical, QByteArrayLiteral("still"));
        appendField(canonical, node->mediaPath.toUtf8());
        appendField(canonical, QByteArray::number(node->contentRevision));
        return canonical;
    case NodeKind::Generate:
        appendField(canonical, QByteArrayLiteral("generate"));
        appendField(canonical, node->resultDigest.toUtf8());
        appendField(canonical, node->resultPath.toUtf8());
        return canonical;
    default:
        break;
    }

    visiting.insert(nodeId);
    appendField(canonical, QByteArrayLiteral("op"));
    appendField(canonical, QByteArray::number(int(node->kind)));

    const CompositeParams &p = node->comp;
    appendField(canonical, QByteArray::number(int(p.blendMode)));
    appendField(canonical, number(p.opacity));
    appendField(canonical, QByteArray::number(p.invertMask ? 1 : 0));
    appendField(canonical, QByteArray::number(p.lumaKey ? 1 : 0));
    appendField(canonical, p.keyColor.name(QColor::HexArgb).toLatin1());
    appendField(canonical, number(p.keyTolerance));
    appendField(canonical, number(p.keySoftness));
    appendField(canonical, number(p.translateX));
    appendField(canonical, number(p.translateY));
    appendField(canonical, number(p.scale));
    appendField(canonical, number(p.rotationDeg));

    for (const CompositeInput &input : wiredInputs(graph, *node)) {
        appendField(canonical, QByteArray::number(input.matte ? 1 : 0));
        if (input.nodeId == -1)
            appendField(canonical, QByteArrayLiteral("none"));
        else
            appendField(canonical, canonicalForm(graph, input.nodeId, visiting));
    }
    visiting.remove(nodeId);
    return canonical;
}

} // namespace

CompositePlan buildCompositePlan(const NodeGraph &graph, int targetNodeId)
{
    CompositePlan plan;
    plan.targetNodeId = targetNodeId;

    const Node *target = graph.nodeById(targetNodeId);
    if (!target || !producesImage(target->kind))
        return plan;

    QSet<int> visiting;
    QSet<int> done;
    if (!collect(graph, targetNodeId, visiting, done, plan)) {
        plan.sourceNodeIds.clear();
        plan.passes.clear();
        return plan;
    }
    plan.valid = true;
    return plan;
}

QString compositeSignature(const NodeGraph &graph, int nodeId)
{
    QSet<int> visiting;
    const QByteArray canonical = canonicalForm(graph, nodeId, visiting);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

} // namespace cutpilot::core
